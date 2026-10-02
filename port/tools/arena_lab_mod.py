"""Phase 0 load test: writes visibly modified arena files into a test game.

    arena_lab_mod.py <src pac/bg dir> <dst pac/bg dir> [bg00 bg01 ...]

For each arena: the ring apron texture (rin_ep00) becomes a magenta/yellow
checkerboard, and every barrier model (ar_fence*) is raised 15 units
(about 1.5 m). Changed entries are re-packed with the C++ BPE compressor (svrmod.exe): the
game loads entries in place, so packed data must stay smaller than unpacked.
Only the destination folder is written.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import svrfmt as f  # noqa: E402
import jboy  # noqa: E402
import arena_tool as at  # noqa: E402


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def flat_block(fourcc, rgb):
    c = rgb565(*rgb)
    color = struct.pack('<HHI', c, c, 0)
    if fourcc == b'DXT1':
        return color
    if fourcc == b'DXT3':
        return b'\xff' * 8 + color
    return bytes((255, 255, 0, 0, 0, 0, 0, 0)) + color   # DXT5


def checker_dds(dds):
    """Same size, format and mip count, checker pattern (8x8 squares)."""
    h, w = struct.unpack_from('<II', dds, 12)
    mips = max(1, struct.unpack_from('<I', dds, 28)[0])
    fourcc = dds[84:88]
    if fourcc not in (b'DXT1', b'DXT3', b'DXT5'):
        return None
    out = bytearray(dds[:128])
    for level in range(mips):
        lw, lh = max(1, w >> level), max(1, h >> level)
        bw, bh = max(1, (lw + 3) // 4), max(1, (lh + 3) // 4)
        for by in range(bh):
            for bx in range(bw):
                sq = ((bx * 4 << level) // 64 + (by * 4 << level) // 64) % 2
                out += flat_block(fourcc, (255, 0, 255) if sq else (255, 230, 0))
    return bytes(out)


def raise_model(raw, dy):
    m = jboy.read(raw)
    for s in m.meshes:
        s.verts = [(x, y + dy, z, nx, ny, nz, c) for (x, y, z, nx, ny, nz, c) in s.verts]
        cx, cy, cz, r = s.sphere
        s.sphere = (cx, cy + dy, cz, r)
        s.raw = s.raw[:0xA4] + struct.pack('>4f', *s.sphere) + s.raw[0xB4:]
    for n in m.nodes:
        cx, cy, cz, r = n['sphere']
        n['sphere'] = (cx, cy + dy, cz, r)
    return jboy.write(m)


def mod_arena(src, dst, textures=True):
    h, g, t = f.epac_read(open(src, 'rb').read())
    stats = {'tex': 0, 'models': 0}
    new_groups = []
    for typ, ents in g:
        new_ents = []
        for name, blob in ents:
            if blob[:4] != b'PACH':
                new_ents.append((name, blob))
                continue
            out = []
            for eid, b in f.pach_read(blob):
                raw = at.unpack(b)
                new = None
                if textures and at.is_tex_bundle(raw):
                    texs = at.tex_bundle(raw)
                    hit = False
                    for i, (tn, dds) in enumerate(texs):
                        if tn.lower().startswith('rin_ep00'):
                            c = checker_dds(dds)
                            if c:
                                texs[i] = (tn, c)
                                hit = True
                                stats['tex'] += 1
                    if hit:
                        new = at.tex_bundle_write(texs)
                elif raw[:4] == b'JBOY':
                    m = jboy.read(raw)
                    if m.name.lower().startswith('ar_fen'):
                        new = raise_model(raw, 15.0)
                        stats['models'] += 1
                out.append((eid, f.bpe_pack(new) if new is not None else b))
            new_ents.append((name, f.pach_write(out)))
        new_groups.append((typ, new_ents))
    open(dst, 'wb').write(f.epac_write(h, new_groups, t))
    return stats


if __name__ == '__main__':
    src_dir, dst_dir = sys.argv[1], sys.argv[2]
    args = sys.argv[3:]
    textures = '--no-textures' not in args
    names = [a for a in args if not a.startswith('--')] or ['bg%02d' % i for i in range(19)]
    for n in names:
        st = mod_arena(os.path.join(src_dir, n + '.pac'), os.path.join(dst_dir, n + '.pac'), textures)
        print(n, st)
