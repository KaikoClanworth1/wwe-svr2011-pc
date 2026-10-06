"""WWE '13 character model -> a ch.pac SvR2011 can load (superstar mods).

    python w13_char.py <WWE13 chNNN.pac (decoded EPK8)> <out ch.pac> --template <2011 chNNN.pac> [--portrait <256 x 256 DDS>]

WWE '13 models (EPK8 / EMD / PACH / JBOY, BPE) use a 3ds Max Biped skeleton
named "Bip ..." in cm with the floor at 0; SvR2011 uses its own names in 10 cm
units with the pelvis at 0. The rigs are the same (joint positions and rest
pose agree at 0.1 scale), so bones are renamed and re-parented by name, the
vertices scaled, palettes remapped. JBOY meshes are 0xB8 bytes (an extra u32
at +0x74); shaders yCh_* become SvR2011's yBumpMapSS / yReflect / yDefault
with a 2011 model's parameter blocks. The 2011 template gives the skeleton's
node frames and the children WWE '13 lacks (face animation 0x64, 0x50, 0x28,
cube map, tiling noise).
"""
import argparse, io, math, os, struct, sys
import numpy as np
from PIL import Image

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import ch_tool, svrfmt, jboy  # noqa: E402

B = 8

# ---------------------------------------------------------------- reading
def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def cstr(b): return b.split(b'\0')[0].decode('latin-1')


def read13(d):
    """WWE '13 JBOY -> dict (meshes 0xB8)."""
    nmesh, mptr, nnode, ntex, nptr, tptr, optr = struct.unpack_from('>7I', d, 0x18)
    m = {'name': cstr(d[B + optr:B + optr + 16]),
         'textures': [cstr(d[B + tptr + 16 * i:B + tptr + 16 * i + 16]) for i in range(ntex)],
         'nodes': [], 'meshes': []}
    for i in range(nnode):
        o = B + nptr + 80 * i
        m['nodes'].append({'name': cstr(d[o:o + 16]), 't': struct.unpack_from('>3f', d, o + 16),
                           'r': struct.unpack_from('>3f', d, o + 32), 'parent': struct.unpack_from('>i', d, o + 48)[0]})
    for i in range(nmesh):
        o = B + mptr + 0xB8 * i
        vc, nstrip, npal = struct.unpack_from('>3I', d, o)
        s = {'palette': list(struct.unpack_from('>20i', d, o + 0x0C))[:npal]}
        nw = u32(d, o + 0x5C)
        vblk, wptr, uvptr = struct.unpack_from('>3I', d, o + 0x68)
        s['shader'] = cstr(d[o + 0x7C:o + 0x8C])
        s['vfmt'] = u32(d, o + 0x90)
        npar, parptr, iptr = struct.unpack_from('>3I', d, o + 0x94)
        vdat = B + u32(d, B + vblk)
        s['verts'] = [struct.unpack_from('>6fI', d, vdat + 28 * k) for k in range(vc)]
        s['nw'] = max(1, nw)
        s['wraw'] = d[B + wptr:B + wptr + 8 * vc * s['nw']]
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


def read13_nodes(d):
    nnode, nptr = u32(d, 0x20), u32(d, 0x28)
    out = []
    for i in range(nnode):
        o = B + nptr + 80 * i
        out.append({'name': cstr(d[o:o + 16]), 't': struct.unpack_from('>3f', d, o + 16),
                    'r': struct.unpack_from('>3f', d, o + 32), 'parent': struct.unpack_from('>i', d, o + 48)[0]})
    return out


def tex_bundle_read(b):
    n = struct.unpack_from('<I', b, 0)[0]
    out = []
    for i in range(n):
        r = b[16 + 32 * i:48 + 32 * i]
        sz, off = struct.unpack_from('<II', r, 20)
        out.append((r[:16].split(b'\0')[0].decode('latin-1'), b[off:off + sz]))
    return out


def tex_bundle_write(texs, head=None):
    n = len(texs)
    off = (16 + 32 * n + 15) & ~15
    recs, blobs = b'', b''
    for name, dd in texs:
        recs += name.encode('latin-1')[:16].ljust(16, b'\0') + b'dds\0' + struct.pack('<II', len(dd), off + len(blobs)) + b'\0' * 4
        blobs += dd + b'\0' * (-len(dd) % 16)
    hdr = struct.pack('<I', n) + (head[4:16] if head else b'\0' * 12)
    return (hdr + recs).ljust(off, b'\0') + blobs


# ---------------------------------------------------------------- skeleton
def rot(r):
    x, y, z = r
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx


def world(nodes, tkey='t', rkey='r'):
    W = []
    for n in nodes:
        M = np.eye(4)
        M[:3, :3] = rot(n[rkey])
        M[:3, 3] = n[tkey]
        W.append(M if n['parent'] < 0 else W[n['parent']] @ M)
    return W


def name13to11(n):
    """WWE '13 bone -> SvR2011 node (None: keep looking up the parents)."""
    side = None
    for pre, s in (('Bip L ', 'l'), ('Bip R ', 'r'), ('L_', 'l'), ('R_', 'r'), ('l_', 'l'), ('r_', 'r')):
        if n.startswith(pre):
            side, rest = s, n[len(pre):]
            break
    fixed = {'Bip Pelvis': 'root', 'Bip': 'root', 'null': 'root', 'vector': 'root',
             'Bip Spine': 'koshi', 'Bip Spine1': 'mune', 'Bip Neck': 'kubi', 'Neck_x': 'kubi_x', 'Bip Head': 'atama',
             'd_kuchi_t': 'd_kuchi'}
    if n in fixed:
        return fixed[n]
    if n.startswith('kami_b15'):  # (the tie's physics chain, neck to waist: chest, then waist)
        return 'mune' if n in ('kami_b15', 'kami_b15_00', 'kami_b15_01', 'kami_b15_02') else 'koshi'
    if n.startswith('kami_'):  # (hair physics chains: with the head)
        return 'atama'
    if side:
        limb = {'Thigh': 'momo', 'Thigh_u': 'momo', 'Thigh_x': 'momo_x', 'Calf': 'sune', 'Foot': 'ashi', 'Toe0': 'tsumasaki',
                'Clavicle': 'sakotsu', 'Chest_Dummy': 'mune_dummy', 'Clavicle_m': 'sakotsu_m', 'Shoulder_m': 'kata_m',
                'UpperArm': 'ninoude', 'UpperArm_x': 'ninoude_x', 'UpperArm_m': 'ninoude_m', 'Elbow_j': 'kote',
                'Forearm': 'kote', 'Elbow_m': 'kote_m', 'Wrist_x': 'kote_x', 'Wrist_x2': 'kote_x2', 'Hand': 'te'}
        if rest in limb:
            return side + '_' + limb[rest]
        if rest.startswith('Finger'):
            f = rest[6:]
            return '%s_yubi%s%s' % (side, f[0], f[1:] if len(f) > 1 else '0')
    return n  # face bones share names


# bones whose WWE '13 position sets the 2011 node's (one per node)
PRIMARY = {'Bip Pelvis', 'Bip Spine', 'Bip Spine1', 'Bip Neck', 'Neck_x', 'Bip Head', 'd_kuchi_t'}


def is_primary(n13, n11):
    if n13 in PRIMARY:
        return True
    if n13.startswith(('kami_', 'vector', 'null')) or n13 in ('Bip',):
        return False
    if n13.endswith(('Thigh_u', 'Elbow_j')):
        return False
    return True


# ---------------------------------------------------------------- conversion
class Conv:
    def __init__(self, tmpl_pac, portrait=None):
        self.portrait = open(portrait, 'rb').read() if portrait else None
        _, groups = ch_tool.epk8_read(open(tmpl_pac, 'rb').read())
        n, b = groups[0][1][0]
        self.t = {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(b))}
        self.body = jboy.read(self.t[0])
        self.hair = jboy.read(self.t[0x2710])
        self.lash = jboy.read(self.t[0x4e20])
        self.tnodes = self.body.nodes           # jboy.py dicts (written back as read)
        self.tframe = read13_nodes(self.t[0])    # true frames (r at +32, parent at +48)
        self.tindex = {nd['name']: i for i, nd in enumerate(self.tnodes)}
        # parameter blocks by kind
        ms = self.body.meshes
        self.p_skin = next(s for s in ms if s.shader == 'yBumpMapSS' and s.vfmt == 0)
        self.p_eye = next(s for s in ms if s.shader == 'yReflect')
        self.p_hair = self.hair.meshes[-1]
        self.p_lash = self.lash.meshes[0]
        self.ttex = dict(tex_bundle_read(self.t[0xa]))

    def nodes_for(self, m13):
        """2011 node table with the WWE '13 model's proportions, and the 13 -> 11 node map."""
        n13 = m13['nodes']
        W13 = world(n13)
        pelvis = next(i for i, nd in enumerate(n13) if nd['name'] == 'Bip Pelvis')
        origin = W13[pelvis][:3, 3]
        idx13 = {nd['name']: i for i, nd in enumerate(n13)}
        # target world positions for 2011 nodes
        want = {}
        for i, nd in enumerate(n13):
            n11 = name13to11(nd['name'])
            if n11 in self.tindex and is_primary(nd['name'], n11) and n11 not in want:
                want[n11] = 0.1 * (W13[i][:3, 3] - origin)
        nodes = [dict(nd) for nd in self.tnodes]
        Wn = []
        for j, nd in enumerate(nodes):
            fr = self.tframe[j]
            p = fr['parent']
            if nd['name'] in want and p >= 0:
                Pp = Wn[p][:3, 3]
                Rp = Wn[p][:3, :3]
                nd['t'] = tuple(float(x) for x in Rp.T @ (want[nd['name']] - Pp))
            M = np.eye(4)
            M[:3, :3] = rot(fr['r'])
            M[:3, 3] = nd['t']
            Wn.append(M if p < 0 else Wn[p] @ M)
        # 13 -> 11 index map (walking up the parents when a bone has no 2011 node)
        nmap = {}
        for i, nd in enumerate(n13):
            k = i
            while True:
                n11 = name13to11(n13[k]['name'])
                if n11 in self.tindex:
                    nmap[i] = self.tindex[n11]
                    break
                k = n13[k]['parent']
                if k < 0:
                    nmap[i] = 0
                    break
        return nodes, nmap, origin

    def mesh(self, s13, tmpl, nmap, origin, textures, slot_names):
        s = jboy.Mesh()
        s.raw = bytes(tmpl.raw)
        s.u04 = 1
        s.u74 = 0
        s.shader = tmpl.shader
        s.vfmt = tmpl.vfmt
        s.palette = [nmap[p - 1] + 1 if p > 0 else p for p in s13['palette']]
        # (WWE '13 skin keeps shader data in the vertex colour - alpha 0, RGB 0x32 on some attires;
        # 2011 multiplies by it and draws its alpha as opacity: white, as 2011's own models)
        s.verts = [(0.1 * (v[0] - origin[0]), 0.1 * (v[1] - origin[1]), 0.1 * (v[2] - origin[2]), v[3], v[4], v[5], 0xFFFFFFFF)
                   for v in s13['verts']]
        vc = len(s.verts)
        nw = s13['nw']
        w = s13['wraw']
        s.weights = [[(tuple(w[8 * vc * j + 8 * k:8 * vc * j + 8 * k + 4]), struct.unpack_from('>f', w, 8 * vc * j + 8 * k + 4)[0])
                      for k in range(vc)] for j in range(nw)]
        s.uvs = list(s13['uvs'])
        if getattr(self, 'push', None):  # (a cloth piece over the body: a little out along its normals)
            d = self.push
            s.verts = [(v[0] + d * v[3], v[1] + d * v[4], v[2] + d * v[5], v[3], v[4], v[5], v[6]) for v in s.verts]
        s.strips = [(6, idx) for idx in s13['strips']]
        # parameters: the template's, its texture slots pointed at our names
        s.params = []
        for name, typ, val in tmpl.params:
            if typ == 0x0f:
                want = slot_names.get(name)
                k = textures.index(want) if want in textures else -1
                val = struct.pack('>i', k) + val[4:]
            s.params.append((name, typ, val))
        # bounding sphere
        P = np.array([v[:3] for v in s.verts])
        c = (P.min(0) + P.max(0)) / 2
        r = float(np.sqrt(((P - c) ** 2).sum(1)).max())
        md = bytearray(s.raw)
        struct.pack_into('>I', md, 0x8C, tmpl.vfmt)
        struct.pack_into('>4f', md, 0xA4, float(c[0]), float(c[1]), float(c[2]), r)
        s.raw = bytes(md)
        return s

    def model(self, m13, tmpl_model, nodes, nmap, origin, textures, kind_of):
        m = jboy.Model()
        m.header = tmpl_model.header
        m.name = tmpl_model.name
        m.group = tmpl_model.group
        m.name_tail = getattr(tmpl_model, 'name_tail', b'')
        m.textures = textures
        m.nodes = nodes
        m.meshes = []
        names13 = [nd['name'] for nd in m13['nodes']]
        for k, s13 in enumerate(m13['meshes']):
            tmpl, slots = kind_of(s13)
            pal = [names13[p - 1] if 0 < p <= len(names13) else '' for p in s13['palette']]
            self.push = 0.08 if pal and sum(n.startswith('kami_b15') for n in pal) * 2 >= len(pal) else 0
            ms = self.mesh(s13, tmpl, nmap, origin, textures, slots)
            self.push = 0
            ms.u74 = k
            m.meshes.append(ms)
        return m

    def attire(self, kids13):
        """WWE '13 EMD children -> 2011 EMD children."""
        body13 = read13(kids13[0])
        nodes, nmap, origin = self.nodes_for(body13)
        SKIN = {'texDiffuse': 'color', 'texNormal': 'normal', 'texOcclusion': 'mk', 'texTilingMap': 'noise'}
        EYE = {'texDiffuse': 'color', 'texCubeRefction': 'cubemap', 'texRefctionReg': 'color', 'texOcclusion': 'mk'}
        tex_body = ['color', 'normal', 'mk', 'noise', 'cubemap']

        def kind(s):
            if 'Reflect' in s['shader']:
                return self.p_eye, EYE
            return self.p_skin, SKIN
        out = {}
        body = self.model(body13, self.body, nodes, nmap, origin, tex_body, kind)
        out[0] = jboy.write(body)
        # hair / mustache (transA) and eyelash (transB): same skeleton, by name
        for src, dst, tm, slots in ((0x2710, 0x2710, self.hair, {'texDiffuse': 'color', 'texNormal': 'normal', 'texOcclusion': 'mk', 'texTilingMap': 'noise'}),
                                    (0x4e22, 0x4e20, self.lash, {'texDiffuse': 'color'})):
            if src not in kids13 or kids13[src][:4] != b'JBOY':
                continue
            m13 = read13(kids13[src])
            sub_map = {}
            body_names = {b['name']: j for j, b in enumerate(body13['nodes'])}
            idx11 = {nd['name']: j for j, nd in enumerate(nodes)}
            for i, nd in enumerate(m13['nodes']):
                # a body bone by name; else the 2011 node the name maps to; else
                # the nearest ancestor that is one (hair chains hang off the head)
                k = i
                while k >= 0:
                    nm = m13['nodes'][k]['name']
                    if nm in body_names:
                        sub_map[i] = nmap[body_names[nm]]
                        break
                    if name13to11(nm) in idx11:
                        sub_map[i] = idx11[name13to11(nm)]
                        break
                    k = m13['nodes'][k]['parent']
                else:
                    sub_map[i] = idx11.get('atama', 0)
            tmpl = tm.meshes[-1]
            out[dst] = jboy.write(self.model(m13, tm, nodes, sub_map, origin, tex_body if dst == 0x2710 else ['color'],
                                             lambda s, t=tmpl, sl=slots: (t, sl)))
        # textures
        t13 = dict(tex_bundle_read(kids13[0xa]))
        color13 = tex_bundle_read(kids13[0xc])[0][1]
        mk = make_mk(t13.get('Mask') or t13.get('mask'))
        texs = [('normal', t13['normal']), ('noise', self.ttex['noise']), ('Mk', mk)]
        # the template's blood overlays (face / body / hair) and their hair textures
        texs += [(n, d) for n, d in self.ttex.items() if n.lower().startswith('bloodhair')]
        out[0xa] = tex_bundle_write(texs, self.t[0xa][:16])
        out[0xb] = self.t[0xb]
        # hair / eyelash cards: WWE '13 cuts them out with the normal map's
        # alpha, 2011 with the colour map's
        cut = []
        for src in (0x2710, 0x4e22):
            if src in kids13 and kids13[src][:4] == b'JBOY':
                for s13 in read13(kids13[src])['meshes']:
                    for idx in s13['strips']:
                        cut += [(s13['uvs'][a_], s13['uvs'][b_], s13['uvs'][c_]) for a_, b_, c_ in strip_tris(idx)]
        out[0xc] = tex_bundle_write([('color', add_alpha(color13, t13['normal'], cut))], kids13[0xc][:16])
        for k in (0xd, 0xe, 0xf):
            if k in kids13:
                out[k] = kids13[k]
        for k in (0x28, 0x32, 0x50, 0x64):
            out[k] = self.t[k]
        for k in (0x7530, 0x7531, 0x7532):
            if k in self.t:
                out[k] = self.t[k]
        for k in (0xc8, 0xc9):  # the portrait (256 x 256 DXT5): the WWE '13 bust render if given
            out[k] = self.portrait or self.t[k]
        return out


def make_mk(mask_dds):
    """2011 'Mk': R = bruise mask (none), G, B, A from WWE '13's mask."""
    im = Image.open(io.BytesIO(mask_dds)).convert('RGBA')
    r, g, b, a = im.split()
    out = Image.merge('RGBA', (Image.new('L', im.size, 0), g, b, a))
    return dds_encode(out, 'DXT5')


def strip_tris(idx):
    out = []
    for k in range(len(idx) - 2):
        a, b, c = idx[k], idx[k + 1], idx[k + 2]
        if a == b or b == c or a == c:
            continue
        out.append((a, b, c) if k % 2 == 0 else (b, a, c))
    return out


def add_alpha(color_dds, normal_dds=None, cut_tris=()):
    """WWE '13 colour maps are DXT1 (no alpha); 2011's alpha is the
    specular level (a mid value from the brightness) and, on hair cards, the
    cut-out (WWE '13's normal map alpha there)."""
    from PIL import ImageDraw, ImageFilter
    im = Image.open(io.BytesIO(color_dds)).convert('RGB')
    a = im.convert('L').point(lambda v: 40 + v // 3)
    if normal_dds and cut_tris:
        W, H = im.size
        mask = Image.new('L', (W, H), 0)
        dr = ImageDraw.Draw(mask)
        for tri in cut_tris:
            dr.polygon([(u * W, v * H) for u, v in tri], fill=255)
        mask = mask.filter(ImageFilter.MaxFilter(5))
        na = Image.open(io.BytesIO(normal_dds)).convert('RGBA').split()[3].resize((W, H), Image.BILINEAR)
        a = Image.composite(na, a, mask)
    rgba = im.copy()
    rgba.putalpha(a)
    return dds_encode(rgba, 'DXT5')


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


def pack_kids(kids):
    return svrfmt.pach_write([(i, svrfmt.bpe_encode(kids[i], compress=True)) for i in sorted(kids)])


def convert(src, out, tmpl, portrait=None):
    conv = Conv(tmpl, portrait)
    _, groups = ch_tool.epk8_read(open(src, 'rb').read())
    entries = []
    for typ, es in groups:
        for name, blob in es:
            if not name.strip(b'\0'):
                continue
            kids = {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(blob))}
            if 0 not in kids or kids[0][:4] != b'JBOY':
                print('skip', name)
                continue
            print('attire', name.decode(), '...')
            entries.append((typ, name, pack_kids(conv.attire(kids))))
    write_epk8(tmpl, entries, out)
    print('wrote', out, os.path.getsize(out))


def write_epk8(tmpl, entries, out):
    """EPK8 with one EMD group holding the entries (laid out like 2011 ch pacs)."""
    t = open(tmpl, 'rb').read()
    header = bytearray(t[:0x800])
    index = bytearray(0x3800)
    struct.pack_into('<4sHH', index, 0, b'EMD ', 4 * len(entries), 0)  # (the count field is entries * 4)
    # copy the template's group record flags (bytes 6..12)
    index[6:12] = t[0x806:0x80C]
    body = bytearray()
    p = 12
    for typ, name, blob in entries:
        while len(body) % 0x800:
            body.append(0)
        index[p:p + 8] = name.ljust(8, b'\0')[:8]
        struct.pack_into('<II', index, p + 8, len(body) // 0x800, (len(blob) + 0xFF) // 0x100)
        body += blob
        p += 16
    while len(body) % 0x800:
        body.append(0)
    struct.pack_into('<I', header, 4, 12 + 16 * len(entries))  # the index size (the game reads only that much)
    struct.pack_into('<I', header, 8, len(body))
    open(out, 'wb').write(bytes(header) + bytes(index) + bytes(body) + bytes(0x800))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--template', required=True, help='a SvR2011 chNNN.pac: skeleton frames, face animation, cube map (ch104 works)')
    ap.add_argument('--portrait', help='256 x 256 DXT5 DDS (WWE 13 menuHD SSFC/<id>)')
    a = ap.parse_args()
    convert(a.src, a.out, a.template, a.portrait)
