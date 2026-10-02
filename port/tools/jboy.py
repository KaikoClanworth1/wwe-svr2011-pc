"""JBOY (Yuke's YOBJ, big-endian) arena models: read, write, validate.

Layout (all pointers are offsets from byte 8, listed in the POF0 table):

    "JBOY" u32 len | data[len] | "POF0" u32 len | pof bytes (4-aligned)

data (file offsets):
    0x08 u32 0        0x0C u32 len        0x10 u32 0     0x14 u32 0
    0x18 mesh count   0x1C -> meshes      0x20 node count
    0x24 texture count
    0x28 -> nodes     0x2C -> texture names (16 bytes each)
    0x30 -> object name (16 bytes) + u32 a, u32 b
    0x34.. u32 x3 (1, 0, 0 seen)

mesh (0xB4 bytes):
    +00 vertex count  +04 u32 (1)   +08 palette count  +0C palette[20]
    +5C weight block count (2 for crowd people)  +60 u32 (0)  +64 u32 (1)
    +68 -> u32 -> vertices    pos f32x3, normal f32x3, colour ARGB  (28 B)
    +6C -> weights            u8 bone[4] (palette slot, 255 = none), f32  (8 B per
                                 vertex per block)
    +70 -> uvs                f32 u, v                                 (8 B)
    +74 material index  +78 shader name[16]  +88 u32 (6)  +8C u32 vertex format
    +90 param count  +94 -> u32 ptr[param count] -> params
    +98 -> index headers[strip count] {u32 prim (6 = strip), u32 count, -> u16[count]}
    +9C vertex count  +A0 u32  +A4 bounding sphere f32x4
param: name[16], u16 type, u16 size(total), value
    type 0x0d f32x4, 0x0a f32, 0x05 u32, 0x0f texture slot (s32), 0x10 bool
node (80 bytes): name[16], f32 t[3], f32 r[3], u32, s32 parent, u32 x4,
    f32 sphere[4]
"""
import struct

B = 8  # pointer base


def _u32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def _cstr(b):
    # latin-1 keeps every byte (some names are Shift-JIS) so writes round-trip
    return b.split(b'\0')[0].decode('latin-1')


def _name16(n):
    return n.encode('latin-1').ljust(16, b'\0')[:16]


# ------------------------------------------------------------ POF0

def pof_decode(b):
    offs, cur, i = [], 0, 0
    while i < len(b):
        t = b[i] >> 6
        if t == 0:
            break
        if t == 1:
            v = b[i] & 0x3F; i += 1
        elif t == 2:
            v = ((b[i] & 0x3F) << 8) | b[i + 1]; i += 2
        else:
            v = ((b[i] & 0x3F) << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3]; i += 4
        cur += v * 4
        offs.append(cur)
    return offs


def pof_encode(offs):
    out = bytearray()
    cur = 0
    for o in sorted(offs):
        v = (o - cur) // 4
        cur = o
        if v < 0x40:
            out.append(0x40 | v)
        elif v < 0x4000:
            out += bytes((0x80 | (v >> 8), v & 0xFF))
        else:
            out += bytes((0xC0 | (v >> 24), (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))
    out += b'\0' * (-len(out) % 4)   # zero pad doubles as the terminator
    return bytes(out)


# ------------------------------------------------------------ read

class Mesh:
    pass


class Model:
    pass


def read(d):
    if d[:4] != b'JBOY':
        raise ValueError('not JBOY')
    ln = _u32(d, 4)
    pof = d[8 + ln:]
    if pof[:4] != b'POF0':
        raise ValueError('no POF0')
    m = Model()
    m.pointers = pof_decode(pof[8:8 + _u32(pof, 4)])
    m.header = d[8:0x48]
    nmesh, mptr, nnode, ntex, nptr, tptr, optr = struct.unpack_from('>7I', d, 0x18)
    m.name = _cstr(d[B + optr:B + optr + 16])
    m.name_tail = d[B + optr + 16:B + optr + 24]
    m.textures = [_cstr(d[B + tptr + 16 * i:B + tptr + 16 * i + 16]) for i in range(ntex)]
    m.nodes = []
    for i in range(nnode):
        o = B + nptr + 80 * i
        n = {'name': _cstr(d[o:o + 16]),
             't': struct.unpack_from('>3f', d, o + 16),
             'r': struct.unpack_from('>3f', d, o + 28),
             'u40': _u32(d, o + 40),
             'parent': struct.unpack_from('>i', d, o + 44)[0],
             'u48': struct.unpack_from('>4I', d, o + 48),
             'sphere': struct.unpack_from('>4f', d, o + 64)}
        m.nodes.append(n)
    m.meshes = []
    for i in range(nmesh):
        o = B + mptr + 0xB4 * i
        s = Mesh()
        s.raw = d[o:o + 0xB4]
        vc, s.u04, npal = struct.unpack_from('>3I', d, o)
        s.palette = list(struct.unpack_from('>20i', d, o + 0x0C))[:npal]
        s.u5c = struct.unpack_from('>3I', d, o + 0x5C)
        vblk, wptr, uvptr, s.u74 = struct.unpack_from('>4I', d, o + 0x68)
        s.shader = _cstr(d[o + 0x78:o + 0x88])
        s.u88, s.vfmt, npar, parptr, iptr, vc2, s.ua0 = struct.unpack_from('>7I', d, o + 0x88)
        s.sphere = struct.unpack_from('>4f', d, o + 0xA4)
        vdat = B + _u32(d, B + vblk)
        s.verts = [struct.unpack_from('>6fI', d, vdat + 28 * k) for k in range(vc)]
        nw = max(1, s.u5c[0])
        s.weights = []  # per weight block: [(bones u8x4, f32)] per vertex
        for j in range(nw):
            wb = B + wptr + 8 * vc * j
            s.weights.append([(tuple(d[wb + 8 * k:wb + 8 * k + 4]),
                               struct.unpack_from('>f', d, wb + 8 * k + 4)[0]) for k in range(vc)])
        s.uvs = [struct.unpack_from('>2f', d, B + uvptr + 8 * k) for k in range(vc)]
        s.params = []
        for k in range(npar):
            po = B + _u32(d, B + parptr + 4 * k)
            typ, size = struct.unpack_from('>HH', d, po + 16)
            s.params.append((_cstr(d[po:po + 16]), typ, d[po + 20:po + size]))
        s.strips = []  # [(prim, [u16])], s.u04 of them
        for k in range(s.u04):
            prim, icount, idat = struct.unpack_from('>3I', d, B + iptr + 12 * k)
            s.strips.append((prim, list(struct.unpack_from('>%dH' % icount, d, B + idat))))
        s.vc2 = vc2
        # byte ranges, for coverage checks
        s.ranges = [(o, o + 0xB4), (B + vblk, B + vblk + 4), (vdat, vdat + 28 * vc),
                    (B + wptr, B + wptr + 8 * vc * nw), (B + uvptr, B + uvptr + 8 * vc),
                    (B + parptr, B + parptr + 4 * npar), (B + iptr, B + iptr + 12 * s.u04)]
        for k in range(s.u04):
            _, icount, idat = struct.unpack_from('>3I', d, B + iptr + 12 * k)
            s.ranges.append((B + idat, B + idat + 2 * icount))
        for k in range(npar):
            po = B + _u32(d, B + parptr + 4 * k)
            s.ranges.append((po, po + struct.unpack_from('>H', d, po + 18)[0]))
        m.meshes.append(s)
    m.ranges = [(0, 0x48), (B + nptr, B + nptr + 80 * nnode),
                (B + tptr, B + tptr + 16 * ntex), (B + optr, B + optr + 32)]
    m.group = d[B + optr + 16:B + optr + 32]
    m.size = 8 + ln
    return m


def strip_to_tris(idx):
    """Triangle strip (degenerate-joined) -> triangle list."""
    tris = []
    for k in range(len(idx) - 2):
        a, b, c = idx[k], idx[k + 1], idx[k + 2]
        if a == b or b == c or a == c:
            continue
        tris.append((a, b, c) if k % 2 == 0 else (b, a, c))
    return tris


def validate(d):
    """-> list of problems (empty = fully understood)."""
    m = read(d)
    probs = []
    rs = list(m.ranges)
    for i, s in enumerate(m.meshes):
        rs += s.ranges
        vc = len(s.verts)
        for prim, idx in s.strips:
            if idx and max(idx) >= vc:
                probs.append(f'mesh {i}: index {max(idx)} >= {vc}')
            if prim != 6:
                probs.append(f'mesh {i}: prim {prim}')
        if s.vc2 != vc:
            probs.append(f'mesh {i}: vcount {vc} vs {s.vc2}')
        for (bones, w) in s.weights[0]:
            if bones[0] >= max(1, len(s.palette)) and bones[0] != 255:
                probs.append(f'mesh {i}: bone slot {bones[0]} >= palette {len(s.palette)}')
                break
    rs.sort()
    end = 0
    for a, b in rs:
        if a > end:
            gap = d[end:a]
            if any(gap) and a - end >= 16:
                probs.append(f'unclaimed {end:#x}-{a:#x}: {gap[:16].hex()}')
        if a < end and (a, b) != rs[0]:
            pass
        end = max(end, b)
    if end < m.size and any(d[end:m.size].rstrip(b'\0')):
        probs.append(f'unclaimed tail {end:#x}-{m.size:#x}: {d[end:end+16].hex()}')
    return probs


# ------------------------------------------------------------ write

def write(m):
    """Model -> JBOY bytes. Same layout as Yuke's tool: header, mesh
    descriptors, then per mesh [param ptrs, params, vertex header, vertices,
    weights, uvs, index headers, indices], 16-byte pad, nodes, texture
    names, group record. Padding is zero (the originals hold junk there)."""
    nm = len(m.meshes)
    out = bytearray(0x48)
    ptrs = []                     # file offsets that hold a pointer

    def rel(o):
        return o - B

    def put_ptr(at, target):
        struct.pack_into('>I', out, at, rel(target))
        ptrs.append(at)

    mesh_at = len(out)
    out += bytes(0xB4 * nm)
    for i, s in enumerate(m.meshes):
        md = bytearray(s.raw)
        vc = len(s.verts)
        md_at = mesh_at + 0xB4 * i
        # params
        par_list = len(out)
        out += bytes(4 * len(s.params))
        par_at = []
        for name, typ, val in s.params:
            par_at.append(len(out))
            out += _name16(name)
            out += struct.pack('>HH', typ, 20 + len(val)) + val
        # vertices
        vh = len(out)
        out += bytes(4)
        vd = len(out)
        for v in s.verts:
            out += struct.pack('>6fI', *v)
        wd = len(out)
        for blk in s.weights:
            for bones, w in blk:
                out += bytes(bones) + struct.pack('>f', w)
        ud = len(out)
        for uv in s.uvs:
            out += struct.pack('>2f', *uv)
        ih = len(out)
        out += bytes(12 * len(s.strips))
        id_at = []
        for prim, idx in s.strips:
            id_at.append(len(out))
            out += struct.pack('>%dH' % len(idx), *idx)
        out += bytes(-len(out) % 4)
        # descriptor
        struct.pack_into('>3I', md, 0, vc, len(s.strips), len(s.palette))
        pal = list(s.palette) + [-1] * (20 - len(s.palette))
        struct.pack_into('>20i', md, 0x0C, *pal)
        struct.pack_into('>I', md, 0x5C, len(s.weights))
        struct.pack_into('>I', md, 0x74, s.u74)
        md[0x78:0x88] = _name16(s.shader)
        struct.pack_into('>I', md, 0x90, len(s.params))
        struct.pack_into('>I', md, 0x9C, vc)
        out[md_at:md_at + 0xB4] = md
        put_ptr(md_at + 0x68, vh)
        put_ptr(md_at + 0x6C, wd)
        put_ptr(md_at + 0x70, ud)
        put_ptr(md_at + 0x94, par_list)
        put_ptr(md_at + 0x98, ih)
        put_ptr(vh, vd)
        for k, pa in enumerate(par_at):
            put_ptr(par_list + 4 * k, pa)
        for k, (prim, idx) in enumerate(s.strips):
            struct.pack_into('>2I', out, ih + 12 * k, prim, len(idx))
            put_ptr(ih + 12 * k + 8, id_at[k])
    out += bytes(-(len(out) - B) % 16)
    node_at = len(out)
    for n in m.nodes:
        out += _name16(n['name'])
        out += struct.pack('>3f', *n['t']) + struct.pack('>3f', *n['r'])
        out += struct.pack('>Ii', n['u40'], n['parent'])
        out += struct.pack('>4I', *n['u48']) + struct.pack('>4f', *n['sphere'])
    tex_at = len(out)
    for t in m.textures:
        out += _name16(t)
    grp_at = len(out)
    out += _name16(m.name) + m.group
    # header
    out[8:0x48] = m.header
    ln = len(out) - 8
    struct.pack_into('>4s3I', out, 0, b'JBOY', ln, 0, ln)
    struct.pack_into('>I', out, 0x18, nm)
    put_ptr(0x1C, mesh_at)
    struct.pack_into('>2I', out, 0x20, len(m.nodes), len(m.textures))
    put_ptr(0x28, node_at)
    put_ptr(0x2C, tex_at)
    put_ptr(0x30, grp_at)
    pof = pof_encode([rel(p) for p in ptrs])
    return bytes(out) + b'POF0' + struct.pack('>I', len(pof)) + pof


def pad_mask(d):
    """Byte ranges that are padding (junk in originals), for comparisons."""
    m = read(d)
    rs = sorted(m.ranges + [r for s in m.meshes for r in s.ranges])
    gaps, end = [], 0
    for a, b in rs:
        if a > end:
            gaps.append((end, a))
        end = max(end, b)
    return gaps
