"""Repairs character model pacs made before the 13 Oct converter fixes (svr08_char / w13_char /
reskin_port), in place of a full reconversion:

    py fix_model.py <in ch.pac> <out ch.pac> [--check]      -> JSON {fixed, entries: [...]}

1. Mips: Pillow premultiplied alpha when the converters resized RGBA sheets, so the colour sheets' low
   alpha (the specular level) turned the smaller mips' RGB into specks - red dots on the skin at a
   distance. Each DXT5 sheet whose mip 1 is far from its own top level (mean RGB error > 6; stock
   sheets measure <= 3) gets mips 1+ rebuilt from the top level, RGB and alpha resized apart. The top
   level's bytes stay as they are.
2. Tiling maps: a mesh block with its tiling map bound at tile count 1 (a template block whose slot
   was unbound, bound to the noise by the old converters) has it unbound again, as stock.

--check only reports. Entries without a body model, other children and the EPK8 layout are kept.
Run it on converted models only: one stock model (ch195) has a block bound at tile count 1 by Yuke's.
"""
import io
import json
import struct
import sys
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ch_tool  # noqa: E402
import jboy  # noqa: E402
import svrfmt  # noqa: E402
import w13_char as W  # noqa: E402

LIMIT = 6.0


def _bc3(data, w, h):
    """BC3 -> RGBA uint8 (h, w, 4): colour always 4-colour mode, alpha by the DXT5 rules"""
    b = np.frombuffer(data[:16 * max(1, w // 4) * max(1, h // 4)], np.uint8).reshape(-1, 16)
    a0, a1 = b[:, 0].astype(int), b[:, 1].astype(int)
    bits = np.zeros(len(b), np.uint64)
    for k in range(6):
        bits |= b[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
    ai = np.stack([((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(int) for i in range(16)], 1)
    pal = np.zeros((len(b), 8), int)
    pal[:, 0], pal[:, 1] = a0, a1
    big = a0 > a1
    for i in range(1, 7):
        pal[:, i + 1] = np.where(big, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, i + 1] = np.where(big, pal[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(big, pal[:, 6], 0)
    pal[:, 7] = np.where(big, pal[:, 7], 255)
    al = np.take_along_axis(pal, ai, 1)
    c0 = b[:, 8].astype(int) | (b[:, 9].astype(int) << 8)
    c1 = b[:, 10].astype(int) | (b[:, 11].astype(int) << 8)

    def rgb(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1)
    p0, p1 = rgb(c0), rgb(c1)
    cp = np.stack([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3], 1)
    cb = b[:, 12].astype(np.uint32) | (b[:, 13].astype(np.uint32) << 8) | (b[:, 14].astype(np.uint32) << 16) | \
        (b[:, 15].astype(np.uint32) << 24)
    ci = np.stack([(cb >> (2 * i)) & 3 for i in range(16)], 1).astype(int)
    col = np.take_along_axis(cp, ci[..., None].repeat(3, -1), 1)
    return np.concatenate([col, al[..., None]], -1).reshape(h // 4, w // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4) \
        .reshape(h, w, 4).astype(np.uint8)


def mip_error(d):
    """-> (mean RGB error of mip 1 against its top level, w, h, mips) or None"""
    if d[:4] != b'DDS ' or d[84:88] != b'DXT5':
        return None
    h, w = struct.unpack_from('<II', d, 12)
    mips = struct.unpack_from('<I', d, 28)[0]
    if mips < 2 or w < 8 or h < 8:
        return None
    top = Image.fromarray(_bc3(d[128:], w, h), 'RGBA')
    m1 = _bc3(d[128 + 16 * (w // 4) * (h // 4):], w // 2, h // 2).astype(int)
    ref = np.asarray(W.resize_rgba(top, (w // 2, h // 2))).astype(int)
    return float(np.abs(m1[..., :3] - ref[..., :3]).mean()), w, h, mips


def remip(d):
    """mips 1+ rebuilt from the top level (whose bytes stay)"""
    _, w, h, mips = mip_error(d)
    top_len = 16 * (w // 4) * (h // 4)
    top = Image.fromarray(_bc3(d[128:], w, h), 'RGBA')
    out = bytearray(d[:128 + top_len])
    for m in range(1, mips):
        im = W.resize_rgba(top, (max(1, w >> m), max(1, h >> m)))
        b = io.BytesIO()
        im.save(b, 'DDS', pixel_format='DXT5')
        out += b.getvalue()[128:]
    struct.pack_into('<I', out, 20, top_len)
    return bytes(out)


def fix_tiling(model):
    n = 0
    for s in model.meshes:
        p = {nm: (i, v) for i, (nm, t, v) in enumerate(s.params)}
        if 'texTilingMap' in p and 'g_fTileCountU' in p:
            i, v = p['texTilingMap']
            if struct.unpack('>i', v[:4])[0] >= 0 and struct.unpack('>f', p['g_fTileCountU'][1][:4])[0] == 1.0:
                s.params[i] = (s.params[i][0], s.params[i][1], struct.pack('>i', -1) + v[4:])
                n += 1
    return n


def fix_pac(src, out=None):
    raw = open(src, 'rb').read()
    _, groups = ch_tool.epk8_read(raw)
    entries, report, changed = [], [], False
    for name, blob in groups[0][1]:
        if not name.strip(b'\0'):
            continue
        kids = {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(blob))}
        rep = {'entry': name.decode('latin-1'), 'remipped': [], 'tiling_unbound': 0}
        if 0 in kids and kids[0][:4] == b'JBOY':
            for key in sorted(kids):
                d = kids[key]
                if key in (0, 0x2710, 0x4e20) or d[:4] in (b'JBOY', b'PACH') or len(d) < 48 or d[32:36] != b'dds\0':
                    continue
                texs = W.tex_bundle_read(d)
                new, hit = [], False
                for n, t in texs:
                    e = mip_error(t)
                    if e and e[0] > LIMIT and n.lower() != 'noise':
                        rep['remipped'].append('%x/%s %.1f' % (key, n, e[0]))
                        t, hit = remip(t), True
                    new.append((n, t))
                if hit:
                    kids[key] = W.tex_bundle_write(new, d[:16])
            for key in (0, 0x2710, 0x4e20):
                if key in kids and kids[key][:4] == b'JBOY':
                    m = jboy.read(kids[key])
                    k = fix_tiling(m)
                    if k:
                        rep['tiling_unbound'] += k
                        kids[key] = jboy.write(m)
        if rep['remipped'] or rep['tiling_unbound']:
            changed = True
            entries.append((b'EMD ', name, W.pack_kids(kids)))
        else:
            entries.append((b'EMD ', name, blob))
        report.append(rep)
    if out and changed:
        W.write_epk8(src, entries, out)
    return {'fixed': changed, 'entries': report}


def main(argv):
    if len(argv) < 3 and not (len(argv) == 3 and argv[2] == '--check'):
        print(__doc__)
        return 2
    check = '--check' in argv
    args = [a for a in argv[1:] if a != '--check']
    r = fix_pac(args[0], None if check else args[1])
    print(json.dumps(r, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
