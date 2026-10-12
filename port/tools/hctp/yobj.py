"""HCTP (PS2) YOBJ models and RTX3 texture banks.

YOBJ (little-endian, pointers are offsets from byte 8, listed in POF0):
  0x18 mesh count  0x1C node count  0x20 texture count  0x24 -> meshes
  0x28 -> nodes  0x2C -> texture names (16 B)  0x30 -> object name
mesh (0x40): +00 batch count, +04 submesh count, +08 -> batches, +0C -> submeshes,
  +18 -> ?, +20 ?, +28 vertex total, +30 sphere f32x4
batch (0x20): u32 count, u32 ?, -> positions (f32x4, VIF V4_32), -> normals,
  u32 node, -1 x3.  Vertex indices run over the batches in order.
node (0x50): name[16], t f32x3, f32 1, r f32x3 (radians), ?, s32 parent, ..., sphere
submesh (0xD0): material (+0x28 texture slot) then at +0xC0:
  u16 1, u16 qwords, u32 packets, -> packet table, -> end
packet entry (16 B): u32 3, u32 3, u32 verts, -> data (after the GIF tag):
  per vertex 2 qwords: (s, t, q, index), (r, g, b, a); one triangle strip.

RTX3 bank (stage 00200): u32 n, ..., n x {name[16], "txc", u32 size, u32 off, pad}
RTX3: "RTX3", u32 size, u64 TEX0, ..., +0x20 u16 w,h, +0x24 pixel bytes,
  +0x28 pixel offset, +0x38 clut bytes?, +0x3C clut offset. PSMT8 (0x13) / PSMT4
  (0x14), CLUT CT32 CSM1, alpha 0..0x80. Pixels linear.
"""
import math
import struct

import numpy as np

B = 8


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def cstr(b):
    return b.split(b'\0')[0].decode('latin-1')


def euler(r):
    rx, ry, rz = r
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    X = np.array([[1, 0, 0], [0, cx, sx], [0, -sx, cx]])
    Y = np.array([[cy, 0, -sy], [0, 1, 0], [sy, 0, cy]])
    Z = np.array([[cz, sz, 0], [-sz, cz, 0], [0, 0, 1]])
    return X @ Y @ Z   # row vectors: v' = v @ R


class Model:
    pass


def read(d, apply_nodes=False):
    assert d[:4] == b'YOBJ'
    nmesh, nnode, ntex = u32(d, 0x18), u32(d, 0x1C), u32(d, 0x20)
    pm = u32(d, 0x24) + B
    M = Model()
    M.textures = [cstr(d[u32(d, 0x2C) + B + 16 * i:][:16]) for i in range(ntex)]
    M.name = cstr(d[u32(d, 0x30) + B:][:16])
    pn = u32(d, 0x28) + B
    M.nodes = []
    world = []
    for i in range(nnode):
        e = pn + 0x50 * i
        t = struct.unpack_from('<3f', d, e + 16)
        r = struct.unpack_from('<3f', d, e + 32)
        parent = struct.unpack_from('<i', d, e + 48)[0]
        R = euler(r)
        T = np.eye(4)
        T[:3, :3] = R
        T[3, :3] = t
        if 0 <= parent < i:
            T = T @ world[parent]
        world.append(T)
        M.nodes.append(dict(name=cstr(d[e:e + 16]), t=t, r=r, parent=parent,
                            sphere=struct.unpack_from('<4f', d, e + 64)))
    M.world = world
    M.meshes = []
    for mi in range(nmesh):
        e = pm + 0x40 * mi
        nb, nsub = u32(d, e), u32(d, e + 4)
        pb = u32(d, e + 8) + B
        pos, nrm, vnode = [], [], []
        for bi in range(nb):
            cnt, _, pp, pnn, node = struct.unpack_from('<5I', d, pb + 0x20 * bi)
            P = np.frombuffer(d, '<f4', cnt * 4, pp + B).reshape(cnt, 4)[:, :3].astype(np.float64)
            N = np.frombuffer(d, '<f4', cnt * 4, pnn + B).reshape(cnt, 4)[:, :3].astype(np.float64)
            if apply_nodes and node < len(world):
                W = world[node]
                P = P @ W[:3, :3] + W[3, :3]
                N = N @ W[:3, :3]
            pos.append(P)
            nrm.append(N)
            vnode.append(np.full(cnt, max(0, node - 1), np.int32))   # (batch node numbers are 1-based)
        mesh = dict(pos=np.concatenate(pos) if pos else np.zeros((0, 3)),
                    nrm=np.concatenate(nrm) if nrm else np.zeros((0, 3)), subs=[],
                    node=np.concatenate(vnode) if vnode else np.zeros(0, np.int32),
                    sphere=struct.unpack_from('<4f', d, e + 0x30))
        ps = u32(d, e + 0xC) + B
        for si in range(nsub):
            s = ps + 0xD0 * si
            npk = u32(d, s + 0xC4)
            pt = u32(d, s + 0xC8) + B
            strips = []
            for k in range(npk):
                _, _, nvk, pdata = struct.unpack_from('<4I', d, pt + 16 * k)
                pdata += B
                verts = []
                for j in range(nvk):
                    s_, t_, q_ = struct.unpack_from('<3f', d, pdata + 32 * j)
                    idx = u32(d, pdata + 32 * j + 12)
                    col = struct.unpack_from('<4f', d, pdata + 32 * j + 16)
                    verts.append((idx, (s_, t_, q_), col))
                strips.append(verts)
            mesh['subs'].append(dict(tex=u32(d, s + 0x28), strips=strips, mat=d[s:s + 0xC0]))
        M.meshes.append(mesh)
    return M


def strip_tris(n):
    for j in range(2, n):
        yield (j - 1, j - 2, j) if j % 2 else (j - 2, j - 1, j)


# ------------------------------------------------------------ textures

def texbank(d):
    n = u32(d, 0)
    out = {}
    for i in range(n):
        e = d[0x10 + 0x20 * i:0x30 + 0x20 * i]
        name = cstr(e[:16])
        size, off = struct.unpack_from('<II', e, 20)
        try:
            out[name] = rtx3(d[off:off + size + 0x40])
        except Exception as ex:  # noqa
            out[name] = None
    return out


def rtx3(t):
    """-> RGBA uint8 array (h, w, 4)"""
    assert t[:4] == b'RTX3'
    h = struct.unpack_from('<16I', t)
    tex0 = h[2] | (h[3] << 32)
    psm = (tex0 >> 20) & 0x3F
    w, hh = h[8] & 0xFFFF, h[8] >> 16
    po, co = h[10], h[15]
    if psm == 0x13:
        idx = np.frombuffer(t, np.uint8, w * hh, po)
        pal = np.frombuffer(t, np.uint8, 1024, co).reshape(256, 4).copy()
        i = np.arange(256)
        pal = pal[(i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)]
    elif psm == 0x14:
        b = np.frombuffer(t, np.uint8, w * hh // 2, po)
        idx = np.empty(w * hh, np.uint8)
        idx[0::2] = b & 15
        idx[1::2] = b >> 4
        pal = np.frombuffer(t, np.uint8, 64, co).reshape(16, 4).copy()
    elif psm == 0x00:
        img = np.frombuffer(t, np.uint8, w * hh * 4, po).reshape(hh, w, 4).copy()
        img[..., 3] = np.minimum(img[..., 3].astype(int) * 2, 255)
        return img
    else:
        raise ValueError(f'psm {psm:#x}')
    pal = pal.astype(np.int32)
    pal[:, 3] = np.minimum(pal[:, 3] * 2, 255)
    return pal[idx].reshape(hh, w, 4).astype(np.uint8)
