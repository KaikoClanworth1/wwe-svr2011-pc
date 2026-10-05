"""SvR 2008 Parking Lot (bg56) -> a variant SvR 2011 bg78 whose parking room is the 2008 lot.

    python tools/svr08_stage.py <2008 bg56.pac> <2011 bg78.pac> <out bg78.pac> [--report]
        [--png <file.png>] [--drop-2011-cars] [--margin <bytes>] [--footprint x0,x1,z0,z1]
        [--keep-rooms] [--car-blocks <2008 gm.pac>] [--offset x,z]

The variant is loaded only for matches in the parking room (rule 0x1B), so by
default (parking-only) it also gives up what such a match never shows:
  * the Road to WrestleMania rooms (models 100-139, CFRS rooms 6/7) become
    stubs (one zero-area triangle, no textures; the ids stay, because story
    code looks up 100-102, 115, 120-122 without a null check), and the
    textures only they use are removed;
  * textures used only by the GM office and locker rooms (models 20-99) become
    4x4 placeholders under the same name and format (models and collision
    stay). Kept as they are: textures of always-shown models (corridor,
    interview, exit 140-199, TVs 801/803), screens (tv/titan/moni/screen/
    movie/vision), cube maps, and textures no model names (code may use them).
--keep-rooms keeps every other room byte for byte instead.

What the variant changes:
  * Models 0-19 (the parking room, CFRS room 8): 2011's 20 parking models are
    removed. bg56's 19 models go in: bg56 ids 0-15, 17, 18 keep their ids,
    bg_pylon01 16 -> 12 and bg_box_shad 19 -> 16 (the game hides parking
    models 6, 7, 11 and 16 in some cases; those ids now hold small decals).
    They are moved so that the centre of the 2008 fight enclosure lands on the
    parking room's fight-box centre (125, 0, -410), with no rotation.
    Triangles whose centre is outside the parking room's footprint
    (x -60..530, z -780..-322) are cut out of the strips, so the outdoor
    ground does not reach into the corridor or other rooms. Mesh and node
    spheres are recomputed. Texture lists hold only the textures the meshes
    use; ent_ref00 (2008 cube map) is mapped to 2011's ar_ref (bundle 401).
  * Texture bundle 400: textures no longer needed are removed (2011's
    parking-only 78_par_*, and in parking-only mode the RTWM rooms' own), the
    2008 textures the kept meshes use are added. Only 2008 textures are shrunk
    (by dropping their top mip: props down to 128 first, then the ground, then the rest) until the bundle, the
    unpacked total and the file are no bigger than shipped (minus --margin).
  * Collision pair 0x3EB/0x3EC (1003 index, 1004 triangles): the parking
    room's walls are replaced by bg56's 74 triangles (moved the same way). The
    collision of 2011's three gimmick cars stays (the cars come from gm.pac
    GMGB/78PK) unless --drop-2011-cars. The index is rebuilt. The outline
    pair 0 (995/996) and the other rooms' pairs are kept.
  * --car-blocks <2008 gm.pac>: solid boxes (4 walls = 8 triangles, like
    2011's car blocks: 110 high, normals inward) for every 2008 vehicle body
    (car_* and the tire stack, ids below 1500, not the small parts), from its model's x/z bounds at its locator
    (GMGA/0056 locator = the T23 record's position in the record list), with
    the 2008 rotation (x' = x cos r + z sin r, z' = -x sin r + z cos r), moved
    by the stage offset. In 2008 the enclosure walls (bg56's collision) run
    along the car fronts and the cars stand outside them; there is no other
    car collision in 2008 (not in the gimmick package, and bg56's second pair
    997/999 is a copy of 995/996). svr08_gimmick.py stores the locators'
    y rotation negated, because 2011 turns objects the other way.
  * 50001 (model flags): the lines for ids 0-19 come from bg56, with 'n'
    (near clip) added like every bg78 model, and the 2008 light numbers l(..)
    dropped (bg78 has no lights 110/120).

--offset x,z moves the 2008 lot by (x, 0, z) instead (svr08_gimmick.py takes
the same option; use the same value for both). The report gives the
clearance of every collision triangle from rule 0x1B's two start spots
(START_SPOTS, from the game), and --png marks them.

HMD format (verified: the shipped indexes are rebuilt byte for byte):
  "HMD " u32 n, then n x 96-byte triangles: float M[4][4] (world -> local,
  row vectors: local = w.R + t; M[0][3]/M[1][3] = world X range of the
  triangle, M[2][3] = 0, M[3][3] = 1), 3 corners (local x, local z), a radius
  and 0. Index entry: 200 x {u16 start, u16 count} + u16 triangle list;
  cell = 2*floor(|z|/10) + (z<0) over the triangle's z range.
"""
import math
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svrfmt as f  # noqa: E402
import jboy  # noqa: E402
import arena_tool as at  # noqa: E402

PARK_IDS = range(0, 20)                  # CFRS room 8 = the parking room
PARK_INDEX, PARK_HMD = 1003, 1004        # 0x3EB / 0x3EC
BOX_CENTRE = (125.0, -410.0)             # sub_8224EF28, parking (x, z)
FOOTPRINT = (-60.0, 530.0, -780.0, -322.0)   # 2011 parking floor (x0, x1, z0, z1); corridor starts at z -320
CAR_AREA = (50.0, 270.0, -510.0, -400.0)     # 2011 gimmick cars' collision (inside the room walls)
REFLECTION_MAP = {'ent_ref00': 'ar_ref'}
ID_MAP = {16: 12, 19: 16}                # bg56 id -> variant id (code hides 6, 7, 11, 16 at times)
GROUND = (0, 1, 2, 3, 4, 5)              # bg56 ground, floor and line models: their textures shrink last
RTWM_IDS = range(100, 140)               # training room + green room (Road to WrestleMania only)
ROOM_IDS = range(20, 100)                # GM office, locker rooms A/B/large
ALWAYS_SHOWN = set(range(140, 200)) | {801, 803}
SCREEN_WORDS = ('tv', 'titan', 'moni', 'screen', 'movie', 'vision')
START_SPOTS = ((150.0, -450.0), (125.0, -450.0))   # rule 0x1B: wrestler 1, 2 (x, z)


def offset_for(R56, offset=None):
    """(dx, dz): the given offset, or the 2008 enclosure centre -> fight-box centre."""
    if offset:
        return float(offset[0]), float(offset[1])
    P = np.array([c for r in R56 for c in tri_corners(r)])
    cx, cz = (P[:, 0].min() + P[:, 0].max()) / 2, (P[:, 2].min() + P[:, 2].max()) / 2
    return round(BOX_CENTRE[0] - cx, 1), round(BOX_CENTRE[1] - cz, 1)


def point_tri_dist(px, pz, corners):
    """Top-down distance from a point to a triangle (0 inside)."""
    pts = [(c[0], c[2]) for c in corners]

    def seg(a, b):
        ax, az = a
        bx, bz = b
        vx, vz = bx - ax, bz - az
        ln = vx * vx + vz * vz
        t = 0.0 if ln < 1e-9 else max(0.0, min(1.0, ((px - ax) * vx + (pz - az) * vz) / ln))
        return math.hypot(ax + t * vx - px, az + t * vz - pz)
    d = min(seg(pts[i], pts[(i + 1) % 3]) for i in range(3))
    s = [(pts[(i + 1) % 3][0] - pts[i][0]) * (pz - pts[i][1]) - (pts[(i + 1) % 3][1] - pts[i][1]) * (px - pts[i][0])
         for i in range(3)]
    if all(v >= 0 for v in s) or all(v <= 0 for v in s):
        area = abs((pts[1][0] - pts[0][0]) * (pts[2][1] - pts[0][1]) - (pts[2][0] - pts[0][0]) * (pts[1][1] - pts[0][1]))
        if area > 1e-6:
            return 0.0
    return d


# ------------------------------------------------------------ containers

def unpack(b):
    return f.bpe_decode(b) if b[:4] == b'BPE ' else b


def stage(path):
    """-> (header, groups, trailer, (group, entry) of the STG PACH, [(id, stored)])"""
    h, g, t = f.epac_read(open(path, 'rb').read())
    for gi, (typ, ents) in enumerate(g):
        for ei, (name, blob) in enumerate(ents):
            if typ == b'STG ' and name[:1] == b'0' and unpack(blob)[:4] == b'PACH':
                return h, g, t, (gi, ei), f.pach_read(unpack(blob))
    raise SystemExit(f'{path}: no STG/00NN stage entry')


# ------------------------------------------------------------ HMD collision

def hmd_read(u):
    n = struct.unpack_from('<I', u, 4)[0]
    return [np.array(struct.unpack_from('<24f', u, 8 + 96 * k), dtype=np.float64) for k in range(n)]


def hmd_write(recs):
    return b'HMD ' + struct.pack('<I', len(recs)) + b''.join(struct.pack('<24f', *r) for r in recs)


def tri_corners(r):
    M = r[:16].reshape(4, 4)
    Ri = np.linalg.inv(M[:3, :3])
    t = M[3, :3]
    return [(np.array([a, 0.0, b]) - t) @ Ri for a, b in r[16:22].reshape(3, 2)]


def tri_xrange(r):
    xs = [c[0] for c in tri_corners(r)]
    return min(xs), max(xs)


def _cells(z0, z1, size=10.0):
    out, band = set(), (lambda z: int(math.floor(abs(z) / size)))
    if z1 >= 0:
        out.update(2 * k for k in range(band(max(z0, 0.0)), band(z1) + 1))
    if z0 < 0:
        out.update(2 * k + 1 for k in range(band(min(z1, 0.0)), band(z0) + 1))
    return out


def index_build(recs):
    cells = [[] for _ in range(200)]
    for k, r in enumerate(recs):
        zs = [c[2] for c in tri_corners(r)]
        for c in sorted(_cells(min(zs), max(zs))):
            if c < 200:
                cells[c].append(k)
    pairs, lst = [], []
    for c in cells:
        pairs += [len(lst), len(c)]
        lst += c
    return struct.pack(f'<{len(pairs)}H', *pairs) + struct.pack(f'<{len(lst)}H', *lst)


def rot_matrix(deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, 0, -s], [0, 1, 0], [s, 0, c]])   # row vectors: w' = w.Q


def hmd_move(r, Q, d):
    r = r.copy()
    M = r[:16].reshape(4, 4).copy()
    R2 = Q.T @ M[:3, :3]
    M[:3, :3] = R2
    M[3, :3] = M[3, :3] - d @ R2
    r[:16] = M.flatten()
    r[3], r[7] = tri_xrange(r)
    return r


# ------------------------------------------------------------ models

def sphere_of(points):
    P = np.array(points)
    lo, hi = P.min(0), P.max(0)
    c = (lo + hi) / 2
    return (float(c[0]), float(c[1]), float(c[2]), float(np.sqrt(((P - c) ** 2).sum(1)).max()) + 0.01)


def _orient(t):
    """A triangle with its smallest index first (same winding)."""
    k = min(range(3), key=lambda i: t[i])
    return tuple(t[k:]) + tuple(t[:k])


def strip_filter(idx, keep_tri):
    """The strip with the dropped triangles cut out. Runs of kept triangles
    are copied as they are and joined by degenerates, each run starting at a
    position of the same parity as in the source, so windings are kept."""
    n = len(idx) - 2
    state = []
    for k in range(n):
        a, b, c = idx[k], idx[k + 1], idx[k + 2]
        if a == b or b == c or a == c:
            state.append(None)
        else:
            state.append(keep_tri((a, b, c) if k % 2 == 0 else (b, a, c)))
    runs, k = [], 0
    while k < n:
        if state[k] is False:
            k += 1
            continue
        k0 = k
        while k < n and state[k] is not False:
            k += 1
        if any(state[j] for j in range(k0, k)):
            runs.append((k0, k - 1))
    out = []
    for k0, k1 in runs:
        seg = list(idx[k0:k1 + 3])
        if out:
            out += [out[-1], seg[0]]
        if len(out) % 2 != k0 % 2:
            out.append(seg[0])
        out += seg
    return out


def move_model(raw, Q, d, footprint):
    """Moves a static (single-node, world-space) model, drops triangles whose
    centroid is outside footprint (cut out of the original strips), compacts
    vertices of cut meshes, recomputes spheres.
    Returns (model or None, kept tris, dropped tris)."""
    m = jboy.read(raw)
    x0, x1, z0, z1 = footprint
    kept_meshes, kept, dropped = [], 0, 0
    for s in m.meshes:
        V = np.array([v[:3] for v in s.verts]) @ Q + d
        N = np.array([v[3:6] for v in s.verts]) @ Q

        def inside(t):
            cx, cz = V[list(t), 0].mean(), V[list(t), 2].mean()
            return bool(x0 <= cx <= x1 and z0 <= cz <= z1)

        before, strips = [], []
        for prim, idx in s.strips:
            if prim != 6:
                raise SystemExit(f'{m.name}: primitive {prim} not handled')
            before += jboy.strip_to_tris(idx)
            new = strip_filter(idx, inside)
            if new:
                strips.append((prim, new))
        want = sorted(_orient(t) for t in before if inside(t))
        got = sorted(_orient(t) for prim, idx in strips for t in jboy.strip_to_tris(idx))
        if want != got:
            raise SystemExit(f'{m.name}: strip filter lost triangles')
        kept += len(want)
        dropped += len(before) - len(want)
        if not strips:
            continue
        s.verts = [(*map(float, V[o]), *map(float, N[o]), s.verts[o][6]) for o in range(len(s.verts))]
        if len(want) < len(before):          # cut: drop the vertices no triangle uses
            used = sorted({i for prim, idx in strips for i in idx})
            remap = {o: n for n, o in enumerate(used)}
            s.verts = [s.verts[o] for o in used]
            s.uvs = [s.uvs[o] for o in used]
            s.weights = [[blk[o] for o in used] for blk in s.weights]
            strips = [(prim, [remap[i] for i in idx]) for prim, idx in strips]
        s.strips = strips
        s.sphere = sphere_of([v[:3] for v in s.verts])
        s.raw = s.raw[:0xA4] + struct.pack('>4f', *s.sphere) + s.raw[0xB4:]
        kept_meshes.append(s)
    if not kept_meshes:
        return None, kept, dropped
    m.meshes = kept_meshes
    allp = [v[:3] for s in m.meshes for v in s.verts]
    for n in m.nodes:
        # vertices are world space (spheres agree); a node's r is the exporter's
        # object rotation, kept as is (2011 bg78 has such nodes too)
        if any(abs(x) > 1e-4 for x in n['t']) or len(m.nodes) != 1:
            raise SystemExit(f'{m.name}: not a static single-node model')
        n['sphere'] = sphere_of(allp)
    return m, kept, dropped


def stub_model(raw):
    """A model kept only so lookups by id find it: its first mesh with one
    zero-area triangle and no textures."""
    m = jboy.read(raw)
    s = m.meshes[0]
    v = s.verts[0]
    s.verts = [v, v, v]
    s.uvs = [s.uvs[0]] * 3
    s.weights = [[blk[0]] * 3 for blk in s.weights]
    s.strips = [(6, [0, 1, 2])]
    s.params = [(n, t, struct.pack('>i', -1) + val[4:]) if t == 0x0F else (n, t, val) for n, t, val in s.params]
    s.sphere = (v[0], v[1], v[2], 0.01)
    s.raw = s.raw[:0xA4] + struct.pack('>4f', *s.sphere) + s.raw[0xB4:]
    m.meshes = [s]
    m.textures = []
    for n in m.nodes:
        n['sphere'] = s.sphere
    return jboy.write(m)


def texture_slots(m):
    """[(mesh, param index, slot)] for every texture param."""
    out = []
    for s in m.meshes:
        for k, (name, typ, val) in enumerate(s.params):
            if typ == 0x0F:
                out.append((s, k, struct.unpack('>i', val[:4])[0]))
    return out


def retexture(m, rename):
    """Texture list = only the names meshes use (renamed through `rename`)."""
    names = []
    for s, k, slot in texture_slots(m):
        if 0 <= slot < len(m.textures):
            n = rename.get(m.textures[slot], m.textures[slot])
            if n not in names:
                names.append(n)
    for s, k, slot in texture_slots(m):
        if 0 <= slot < len(m.textures):
            n = rename.get(m.textures[slot], m.textures[slot])
            name, typ, val = s.params[k]
            s.params[k] = (name, typ, struct.pack('>i', names.index(n)) + val[4:])
    m.textures = names
    return names


# ------------------------------------------------------------ textures

BLOCK = {b'DXT1': 8, b'DXT3': 16, b'DXT5': 16}


def dds_info(d):
    h, w, _, _, mips = struct.unpack_from('<5I', d, 12)
    return w, h, max(1, mips), d[84:88]


def dds_halve(d):
    """Drops the top mip level (exact; every 2008 texture has a mip chain)."""
    w, h, mips, fcc = dds_info(d)
    if fcc not in BLOCK or mips < 2 or w <= 32 or h <= 32:
        return None
    top = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * BLOCK[fcc]
    nw, nh = max(1, w // 2), max(1, h // 2)
    hdr = bytearray(d[:128])
    struct.pack_into('<3I', hdr, 12, nh, nw, max(1, (nw + 3) // 4) * max(1, (nh + 3) // 4) * BLOCK[fcc])
    struct.pack_into('<I', hdr, 28, mips - 1)
    return bytes(hdr) + d[128 + top:]


def dds_placeholder(d):
    """4x4, one level, same format (None for cube maps and unknown formats)."""
    if struct.unpack_from('<I', d, 112)[0] & 0x200:       # caps2: cube map
        return None
    fcc = d[84:88]
    if fcc in BLOCK:
        body = (bytes([255]) * 8 if BLOCK[fcc] == 16 else b'') + struct.pack('<HHI', 0x8410, 0x8410, 0)
    else:
        bits = struct.unpack_from('<I', d, 88)[0]
        if bits not in (16, 32):
            return None
        body = bytes([128]) * (bits // 8 * 16)
    hdr = bytearray(d[:128])
    struct.pack_into('<3I', hdr, 12, 4, 4, len(body))
    struct.pack_into('<I', hdr, 28, 1)
    return bytes(hdr) + body


# ------------------------------------------------------------ flags (50001)

def flags_lines(text):
    out = {}
    for line in text.splitlines():
        p = line.split()
        if p and p[0].isdigit():
            out[int(p[0])] = p[1:]
    return out


def build_flags(flags78, flags56, idmap):
    """bg78's 50001 with the lines for 0-19 replaced by bg56's ({variant id: bg56 id})."""
    lines56 = flags_lines(flags56.decode('cp932', 'replace'))
    out = []
    replaced = False
    for line in flags78.split(b'\r\n'):
        p = line.split()
        if p and p[0].isdigit() and int(p[0]) in PARK_IDS:
            if not replaced:
                for i in sorted(idmap):
                    fl = [x for x in lines56.get(idmap[i], []) if not x.startswith('l(')]
                    if 'n' not in fl:
                        fl = ['n'] + fl
                    out.append((f'{i} ' + ' '.join(fl)).encode())
                replaced = True
            continue
        out.append(line)
    return b'\r\n'.join(out)


# ------------------------------------------------------------ build

def box_walls(cx, cz, hx, hz, ry, y=-45.1, hy=54.9):
    """8 HMD triangles: a solid box (centre, half sizes along its own x/z,
    turned by ry the 2008 way), like 2011's car blocks (normals inward)."""
    c, s = math.cos(ry), math.sin(ry)
    ax = np.array([c, 0.0, -s])          # the box's own x axis in world space
    az = np.array([s, 0.0, c])           # its own z axis
    ez = np.array([0.0, -1.0, 0.0])      # local z = up
    centre = np.array([cx, y, cz])
    out = []
    for normal, off, half in ((az, -hz, hx), (-az, hz, hx), (ax, -hx, hz), (-ax, hx, hz)):   # inward normal
        wc = centre + (off * (az if abs(np.dot(normal, az)) > 0.5 else ax))
        ex = np.cross(normal, ez)
        R = np.column_stack([ex, normal, ez])
        t = -wc @ R
        for corners in (((half, hy), (half, -hy), (-half, -hy)), ((-half, -hy), (-half, hy), (half, hy))):
            r = np.zeros(24)
            M = np.zeros((4, 4))
            M[:3, :3] = R
            M[3, :3] = t
            M[3, 3] = 1
            r[:16] = M.flatten()
            r[16:22] = np.array(corners).flatten()
            r[22] = max(half, hy)
            r[3], r[7] = tri_xrange(r)
            out.append(r)
    return out


def car_blocks(gm08, d):
    """Boxes for the 2008 vehicles of gm.pac GMGA/0056, moved by d."""
    h, g, t = f.epac_read(open(gm08, 'rb').read())
    blob = [b for ty, e in g if ty == b'GMGA' for n, b in e if n == b'0056'][0]
    p = {i: unpack(b) for i, b in f.pach_read(unpack(blob))}
    rec = [struct.unpack_from('<4H', p[0], o) for o in range(0, len(p[0]) - 7, 8)]
    W = struct.unpack(f'<{len(p[2]) // 4}I', p[2])
    loc = unpack(dict(f.pach_read(p[3]))[10000])
    models = {i: jboy.read(unpack(b)) for i, b in f.pach_read(p[4]) if unpack(b)[:4] == b'JBOY'}
    out, names = [], []
    for k, (rid, typ, n, off) in enumerate(rec):
        if typ != 23:
            continue
        mid = int(struct.unpack('<f', struct.pack('<I', W[off]))[0])
        m = models.get(mid)
        if not m or mid >= 1500 or not m.name.startswith(('car_', 'n1840')):
            continue                      # bodies only (15xx are break parts)
        x, y, z, _, rx, ry, rz = struct.unpack_from('<3fI3f', loc, 16 + 32 * k)
        V = np.array([v[:3] for s in m.meshes for v in s.verts])
        x0, x1, z0, z1 = V[:, 0].min(), V[:, 0].max(), V[:, 2].min(), V[:, 2].max()
        if (x1 - x0) * (z1 - z0) < 100:   # small attached parts (car_gomi12)
            continue
        mx, mz = (x0 + x1) / 2, (z0 + z1) / 2
        c, s = math.cos(ry), math.sin(ry)
        cx, cz = x + mx * c + mz * s + d[0], z - mx * s + mz * c + d[2]
        out += box_walls(cx, cz, (x1 - x0) / 2, (z1 - z0) / 2, ry)
        names.append(m.name)
    return out, names


def build(p56, p78, out_path, drop_cars=False, margin=64 * 1024, png=None, report=False,
          footprint=FOOTPRINT, parking_only=True, gm08=None, offset=None):
    log = []
    h78, g78, t78, (gi, ei), e78 = stage(p78)
    _, _, _, _, e56 = stage(p56)
    d78 = {i: unpack(b) for i, b in e78}
    d56 = {i: unpack(b) for i, b in e56}
    orig_file = os.path.getsize(p78)
    orig_unpacked = sum(len(u) for u in d78.values())
    orig_400 = len(d78[400])

    # placement: centre of the 2008 enclosure (its collision) -> the fight-box centre
    R56 = hmd_read(d56[996])
    P = np.array([c for r in R56 for c in tri_corners(r)])
    cx, cz = (P[:, 0].min() + P[:, 0].max()) / 2, (P[:, 2].min() + P[:, 2].max()) / 2
    Q = rot_matrix(0)
    dx_, dz_ = offset_for(R56, offset)
    d = np.array([dx_, 0.0, dz_])
    log.append(f'offset: 2008 (x, z) + ({d[0]:.1f}, {d[2]:.1f}), y + 0, rotation 0 '
               f'(enclosure centre {cx:.1f}, {cz:.1f} -> {cx + d[0]:.1f}, {cz + d[2]:.1f}; '
               f'x {P[:, 0].min() + d[0]:.1f}..{P[:, 0].max() + d[0]:.1f}, z {P[:, 2].min() + d[2]:.1f}..{P[:, 2].max() + d[2]:.1f})')

    # which models stay as they are, which become stubs
    stubs = {i: stub_model(d78[i]) for i in RTWM_IDS if parking_only and d78.get(i, b'')[:4] == b'JBOY'}
    users = {}                                    # texture -> models of the variant that name it
    named = set()
    for i, u in d78.items():
        if u[:4] == b'JBOY':
            for tn in jboy.read(u).textures:
                named.add(tn.lower())
                if i not in PARK_IDS and i not in stubs:
                    users.setdefault(tn.lower(), set()).add(i)
    tex78 = at.tex_bundle(d78[400])
    keep78, removed, placeholders = [], [], []
    for n, dds in tex78:
        base = n.rsplit('.', 1)[0].lower()
        us = users.get(base, set())
        if base in named and not us:
            removed.append((n, len(dds)))           # parking-only (or RTWM-only) texture
            continue
        if (parking_only and us and us <= set(ROOM_IDS) and not any(w in base for w in SCREEN_WORDS)):
            ph = dds_placeholder(dds)
            if ph is not None and len(ph) < len(dds):
                placeholders.append((n, len(dds) - len(ph)))
                keep78.append((n, ph))
                continue
        keep78.append((n, dds))
    names78 = {n.rsplit('.', 1)[0].lower() for n, _ in tex78 + at.tex_bundle(d78[401])}
    log.append(f'2011 textures removed: {len(removed)} ({sum(s for _, s in removed)} bytes)'
               + (f'; 4x4 placeholders: {len(placeholders)} (saves {sum(s for _, s in placeholders)} bytes)'
                  if parking_only else ''))
    if stubs:
        log.append(f'RTWM models {RTWM_IDS.start}-{RTWM_IDS.stop - 1} as stubs: {len(stubs)} '
                   f'(saves {sum(len(d78[i]) - len(s) for i, s in stubs.items())} unpacked bytes)')

    # models
    tex56 = {n.rsplit('.', 1)[0]: dds for n, dds in at.tex_bundle(d56[200])}
    rename = dict(REFLECTION_MAP)
    for n in tex56:
        if n.lower() in names78:          # (none today; a clash would mix up textures)
            rename[n] = ('o8_' + n)[:15]
    models, need, ground_tex, idmap = {}, [], set(), {}
    for i in sorted(k for k in d56 if k in PARK_IDS and d56[k][:4] == b'JBOY'):
        m, kept, dropped = move_model(d56[i], Q, d, footprint)
        if m is None:
            log.append(f'model {i}: every triangle outside the room, left out')
            continue
        names = retexture(m, rename)
        for n in names:
            if n not in need:
                need.append(n)
        if i in GROUND:
            ground_tex.update(names)
        vid = ID_MAP.get(i, i)
        models[vid] = m
        idmap[vid] = i
        log.append(f'model {i:2d}->{vid:2d} {m.name:14s} meshes {len(m.meshes):2d} '
                   f'tris kept {kept:5d} dropped {dropped:5d}')
    inv = {v: k for k, v in rename.items()}
    add, missing = {}, []
    for n in need:
        src = inv.get(n, n)
        if src in tex56 and n not in REFLECTION_MAP.values():
            add[n] = tex56[src]
        elif n.lower() not in names78:
            missing.append(n)
    if missing:
        raise SystemExit(f'textures not found: {missing}')

    # collision
    rec = [hmd_move(r, Q, d) for r in R56]
    cars = []
    if not drop_cars:
        for r in hmd_read(d78[PARK_HMD]):
            c = tri_corners(r)
            if all(CAR_AREA[0] <= p[0] <= CAR_AREA[1] and CAR_AREA[2] <= p[2] <= CAR_AREA[3] for p in c):
                cars.append(r)
    blocks, block_names = car_blocks(gm08, d) if gm08 else ([], [])
    hmd = hmd_write(rec + cars + blocks)
    idx = index_build(rec + cars + blocks)
    log.append(f'collision {PARK_HMD}: {len(rec)} triangles from bg56 + {len(cars)} of 2011 gimmick cars '
               f'+ {len(blocks)} of 2008 vehicle blocks ({", ".join(block_names)}) '
               f'(was {struct.unpack_from("<I", d78[PARK_HMD], 4)[0]})')

    for si, (sx, sz) in enumerate(START_SPOTS):
        near = min(((point_tri_dist(sx, sz, tri_corners(r)), name, k)
                    for name, group in (('wall', rec), ('2011 car', cars), ('car block', blocks))
                    for k, r in enumerate(group)), default=None)
        if near:
            log.append(f'start spot {si + 1} ({sx:.0f}, {sz:.0f}): nearest collision {near[0]:.1f} '
                       f'({near[1]} triangle {near[2]})')

    flags = build_flags(d78[50001], d56[50001], idmap)

    # fit: shrink 2008 textures only, ground last
    shrunk = {}

    def bundle():
        return at.tex_bundle_write(keep78 + [(n + '.dds', dds) for n, dds in add.items()])

    def unpacked_total(b400):
        tot = 0
        for i, u in d78.items():
            if i in PARK_IDS:
                continue
            tot += {400: len(b400), PARK_HMD: len(hmd), PARK_INDEX: len(idx), 50001: len(flags)}.get(
                i, len(stubs[i]) if i in stubs else len(u))
        return tot + sum(len(jboy.write(m)) for m in models.values())

    def halve_one():
        # props down to 128 first, then the ground, then props further
        def tier(n):
            w, h, _, _ = dds_info(add[n])
            return 1 if n in ground_tex else (0 if min(w, h) > 128 else 2)
        for n in sorted(add, key=lambda n: (tier(n), -len(add[n]))):
            h2 = dds_halve(add[n])
            if h2:
                add[n] = h2
                shrunk[n] = shrunk.get(n, 0) + 1
                return True
        return False

    while True:
        b400 = bundle()
        if len(b400) <= orig_400 - margin and unpacked_total(b400) <= orig_unpacked - margin:
            break
        if not halve_one():
            raise SystemExit('cannot fit the textures')

    def fits():
        b = bundle()
        return len(b) <= orig_400 - margin and unpacked_total(b) <= orig_unpacked - margin

    # halving goes in big steps: give back levels while it still fits (ground first)
    orig_add = {n: tex56[inv.get(n, n)] for n in add}
    for n in sorted(shrunk, key=lambda n: (n not in ground_tex, len(orig_add[n]))):
        while shrunk.get(n):
            prev = add[n]
            k = shrunk[n] - 1
            t = orig_add[n]
            for _ in range(k):
                t = dds_halve(t)
            add[n] = t
            if fits():
                shrunk[n] = k
                if not k:
                    del shrunk[n]
            else:
                add[n] = prev
                break
    model_raw = {i: jboy.write(m) for i, m in models.items()}
    model_raw.update(stubs)
    packed = {i: f.bpe_pack(r) for i, r in model_raw.items()}
    small = {PARK_HMD: hmd, PARK_INDEX: idx, 50001: flags}
    for k, v in small.items():
        packed[k] = f.bpe_pack(v)
    while True:
        b400 = bundle()
        packed[400] = f.bpe_pack(b400)
        entries = []
        for i, b in e78:
            if i in PARK_IDS:
                continue
            entries.append((i, packed.get(i, b)))
        entries += [(i, packed[i]) for i in models]
        entries.sort(key=lambda e: e[0])
        groups = [(typ, list(ents)) for typ, ents in g78]
        groups[gi][1][ei] = (groups[gi][1][ei][0], f.pach_write(entries))
        data = f.epac_write(h78, groups, t78)
        if len(data) <= orig_file - margin:
            break
        if not halve_one():
            raise SystemExit('cannot fit the file')
    for i, b in packed.items():
        raw = {**model_raw, **small, 400: b400}[i]
        if len(b) + 64 > len(raw):
            raise SystemExit(f'entry {i} does not compress enough to load in place')
    open(out_path, 'wb').write(data)
    sizes = []
    for n in sorted(add, key=lambda n: (n not in ground_tex, n)):
        w0, h0, _, fcc = dds_info(tex56[inv.get(n, n)])
        w1, h1, _, _ = dds_info(add[n])
        sizes.append(f'{n}{" (ground)" if n in ground_tex else ""} {w0}x{h0} -> {w1}x{h1}')
    log.append(f'textures: 2008 added {len(add)}, shrunk {len(shrunk)}')
    log += ['  ' + s for s in sizes]
    log.append(f'bundle 400 raw {len(b400)} / shipped {orig_400}; unpacked {unpacked_total(b400)} / {orig_unpacked}; '
               f'file {len(data)} / {orig_file}')
    if png:
        draw_png(png, models, rec, cars + blocks, d78)
    if report:
        print('\n'.join(log))
    return log


def draw_png(path, models, rec, cars, d78):
    from PIL import Image, ImageDraw
    x0, x1, z0, z1 = -80, 550, -800, -300
    S = 1.5
    im = Image.new('RGB', (int((x1 - x0) * S), int((z1 - z0) * S)), 'white')
    dr = ImageDraw.Draw(im)
    tp = lambda x, z: ((x - x0) * S, (z1 - z) * S)   # north (corridor) up
    for m in models.values():
        for s in m.meshes:
            for prim, idx in s.strips:
                for t in jboy.strip_to_tris(idx):
                    pts = [tp(s.verts[k][0], s.verts[k][2]) for k in t]
                    dr.line(pts + [pts[0]], fill=(185, 185, 185), width=1)
    for r in hmd_read(d78[996]):
        c = tri_corners(r)
        if max(p[2] for p in c) < z1 and min(p[2] for p in c) > z0:
            dr.line([tp(p[0], p[2]) for p in c] + [tp(c[0][0], c[0][2])], fill=(0, 0, 0), width=1)
    for r, col in [(r, (220, 0, 0)) for r in rec] + [(r, (0, 0, 220)) for r in cars]:
        c = tri_corners(r)
        dr.line([tp(p[0], p[2]) for p in c] + [tp(c[0][0], c[0][2])], fill=col, width=2)
    bx, bz = BOX_CENTRE
    dr.rectangle([tp(bx - 38, bz + 50), tp(bx + 38, bz - 50)], outline=(0, 160, 0))
    for k, (sx, sz) in enumerate(START_SPOTS):
        x, y = tp(sx, sz)
        dr.ellipse([x - 4, y - 4, x + 4, y + 4], fill=(255, 140, 0))
        dr.text((x + 5, y - 6), f'start {k + 1}', fill=(200, 100, 0))
    dr.text((4, 4), 'grey: 2008 models  red: 2008 walls  blue: car blocks  black: outline (pair 0)  '
            'green: fight box  orange: rule 1B start spots  north up', fill=(0, 0, 0))
    im.save(path)


if __name__ == '__main__':
    a = sys.argv[1:]
    pos = [x for x in a if not x.startswith('--')]

    def opt(name):
        if name not in a:
            return None
        v = a[a.index(name) + 1]
        pos.remove(v)
        return v

    png = opt('--png')
    mv = opt('--margin')
    fv = opt('--footprint')
    cb = opt('--car-blocks')
    ov = opt('--offset')
    if len(pos) != 3:
        print(__doc__)
        sys.exit(1)
    build(pos[0], pos[1], pos[2], drop_cars='--drop-2011-cars' in a,
          margin=int(mv) if mv else 64 * 1024, png=png, report='--report' in a,
          footprint=tuple(float(x) for x in fv.split(',')) if fv else FOOTPRINT,
          parking_only='--keep-rooms' not in a, gm08=cb,
          offset=tuple(float(x) for x in ov.split(',')) if ov else None)
