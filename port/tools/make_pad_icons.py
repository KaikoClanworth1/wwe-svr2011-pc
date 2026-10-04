"""PlayStation button pictures for the PC port (src/pad_icons.cpp).

The game's button icons are glyphs of its fonts: each font has an icon page
(a DXT5 texture, U+E000.. in the strings) - four different ones in all (HD /
SD, 512 x 512 and 256 x 512), copied into every font. The PS3 version has the
same pages with PlayStation art at the same places (some wider: START,
SELECT, L2 / R2, L3 / R3).

For each of the four this writes one taller picture (uncompressed RGBA DDS):
  - section 0: the Xbox 360 page as it is,
  - section 1: the PS3 page (same layout),
  - section 2: "Xbox / PlayStation" pairs, for prompts shared by players
    using different controllers;
and pad_icons.txt, the table src/pad_icons.cpp reads: the renderer shows the
taller picture in place of the page (recognized by its data), the fonts' icon
glyphs are moved to section 0 and get PlayStation and pair versions.

    python make_pad_icons.py --ps3 <PS3 USRDIR> --xbox <extracted 360 data> --out <Game Files>

(<extracted 360 data> has pac/menu/fontHD.pac; the PS3 one is the user's own
copy of the game.)
"""

import argparse
import io
import os
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import svrfmt  # noqa: E402

FONT_PACS = ['menu/fontHD.pac', 'menu/fontSD.pac']
BUTTONS = list(range(0xE000, 0xE017)) + list(range(0xE01B, 0xE038))  # (not the logos and stars)


def unbpe(b):
    while b[:4] == b'BPE ':
        b = svrfmt.bpe_decode(b)
    return b


def leaves(path):
    """Every leaf of a .pac: (key, data), PACH levels and texture bundles opened."""
    data = open(path, 'rb').read()
    _, groups, _ = svrfmt.epac_read(data)
    out = []

    def walk(blob, key):
        blob = unbpe(blob)
        if blob[:4] == b'PACH':
            try:
                sub = svrfmt.pach_read(blob)
            except Exception:
                out.append((key, blob))
                return
            for i, b in sub:
                walk(b, f'{key}/{i}')
        elif len(blob) >= 48 and blob[32:35].lower() == b'dds':
            n = struct.unpack_from('<I', blob, 0)[0]
            for i in range(n):
                r = 16 + i * 32
                name = blob[r:r + 16].split(b'\0')[0].decode('latin1')
                size, off = struct.unpack_from('<II', blob, r + 20)
                out.append((f'{key}/{name}', blob[off:off + size]))
        else:
            out.append((key, blob))

    for typ, ents in groups:
        for name, blob in ents:
            walk(blob, f'{typ.decode("latin1")}/{name.decode("latin1")}')
    return out


def icon_table(yfb):
    """The icon sub-font of a .yfb: {code: (x, y, w, h)} (pixels), or None."""
    p = 0
    while True:
        p = yfb.find(b'\x64\x00\x00\x00\x64\x00\x00\x00', p)
        if p < 0 or p + 0x80 > len(yfb):
            return None
        cnt = struct.unpack_from('<I', yfb, p + 0x3C)[0]
        lo, hi = struct.unpack_from('<II', yfb, p + 0x70)
        if 0 < cnt < 2000 and 0xE000 <= lo <= hi < 0x10000:
            codes = struct.unpack_from(f'<{cnt}H', yfb, p + 0x80)
            g = p + 0x80 + ((cnt * 2 + 15) & ~15)
            return {c: struct.unpack_from('<4H', yfb, g + 16 * i) for i, c in enumerate(codes)}
        p += 4


def dds_blocks(dds):
    """A DXT5 DDS's base level: (width, height, block data)."""
    h, w = struct.unpack_from('<II', dds, 12)
    size = max(1, w // 4) * max(1, h // 4) * 16
    return w, h, dds[128:128 + size]


def fnv64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def slash(h):
    """A "/" in the icons' style (white, dark edge), h pixels high."""
    w = max(6, h * 2 // 5)
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    t = max(2, h // 10)
    d.line([(w - t, t), (t, h - t)], fill=(30, 30, 30, 255), width=t + 2)
    d.line([(w - t, t), (t, h - t)], fill=(255, 255, 255, 255), width=t)
    return img


def write_dds_rgba(path, img):
    w, h = img.size
    hdr = bytearray(128)
    struct.pack_into('<4sIIIIIII', hdr, 0, b'DDS ', 124, 0x100F, h, w, w * 4, 0, 1)
    struct.pack_into('<IIIIIIII', hdr, 76, 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    struct.pack_into('<I', hdr, 108, 0x1000)
    with open(path, 'wb') as f:
        f.write(hdr)
        # (B8G8R8A8 in memory, as the masks say)
        r, g, b, a = img.split()
        f.write(Image.merge('RGBA', (b, g, r, a)).tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ps3', required=True, help='the PS3 game\'s USRDIR')
    ap.add_argument('--xbox', required=True, help='the extracted 360 data (with pac/)')
    ap.add_argument('--out', required=True, help='the game folder (pad_icons/ is written there)')
    args = ap.parse_args()

    atlases = {}  # 360 fnv -> dict
    for rel in FONT_PACS:
        x = dict(leaves(os.path.join(args.xbox, 'pac', rel)))
        p = dict(leaves(os.path.join(args.ps3, 'pac', rel)))
        for key, yfb in x.items():
            if not yfb.startswith(b'd\0\0\0d\0\0\0'):
                continue
            tx = icon_table(yfb)
            ty = icon_table(p.get(key, b''))
            if not tx or not ty:
                continue
            # (the font's pages: <entry>/<n-1>/<page>; the icon page is the last DDS)
            parts = key.split('/')
            tex_key = '/'.join(parts[:-1] + [str(int(parts[-1]) - 1)])
            pages = sorted(k for k in x if k.startswith(tex_key + '/') and x[k][:4] == b'DDS ')
            if not pages:
                continue
            page = pages[-1]
            if page not in p or p[page][:4] != b'DDS ':
                continue
            w, h, blocks = dds_blocks(x[page])
            pw, ph, _ = dds_blocks(p[page])
            if (pw, ph) != (w, h):
                continue  # (fontSD F001: a different page on the PS3)
            key_hash = fnv64(blocks)
            if key_hash in atlases:
                continue
            atlases[key_hash] = dict(source=f'{rel}:{page}', w=w, h=h, x360=x[page], ps3=p[page], tx=tx, ty=ty)

    os.makedirs(os.path.join(args.out, 'pad_icons'), exist_ok=True)
    lines = ['# pad_icons.txt - written by tools/make_pad_icons.py (src/pad_icons.cpp reads it)',
             '# atlas <fnv64 of the 360 page> <page w> <page h> <picture h> <file>',
             '# xbox <code>:<x>,<y>,<w>,<h> ...  (Xbox glyphs that identify the page)',
             '# ps <code> <x> <y> <w> <h>      (section 1)',
             '# pair <code> <x> <y> <w> <h>    (section 2: Xbox / PlayStation)']
    for n, (hsh, a) in enumerate(sorted(atlases.items(), key=lambda kv: (kv[1]['w'], kv[1]['source']))):
        w, h = a['w'], a['h']
        x360 = Image.open(io.BytesIO(a['x360'])).convert('RGBA')
        ps3 = Image.open(io.BytesIO(a['ps3'])).convert('RGBA')
        tx, ty = a['tx'], a['ty']
        # Section 2: the pairs, packed in rows.
        pairs = []
        for c in BUTTONS:
            if c not in tx or c not in ty:
                continue
            xr, pr = tx[c], ty[c]
            gx = x360.crop((xr[0], xr[1], xr[0] + xr[2], xr[1] + xr[3]))
            gp = ps3.crop((pr[0], pr[1], pr[0] + pr[2], pr[1] + pr[3]))
            gh = max(xr[3], pr[3])
            s = slash(gh)
            img = Image.new('RGBA', (xr[2] + s.width + pr[2], gh), (0, 0, 0, 0))
            img.paste(gx, (0, (gh - xr[3]) // 2))
            img.paste(s, (xr[2], 0), s)
            img.paste(gp, (xr[2] + s.width, (gh - pr[3]) // 2))
            pairs.append((c, img))
        rows, x0, y0, row_h = [], 0, 0, 0
        placed = []
        for c, img in pairs:
            if x0 + img.width > w:
                x0, y0, row_h = 0, y0 + row_h + 4, 0
            placed.append((c, x0, y0, img))
            x0 += img.width + 4
            row_h = max(row_h, img.height)
        if not placed:
            continue  # (the ticker font's PPV logos: no buttons)
        pair_h = (y0 + row_h + 3) & ~3
        total = h * 2 + pair_h
        tall = Image.new('RGBA', (w, total), (0, 0, 0, 0))
        tall.paste(x360, (0, 0))
        tall.paste(ps3, (0, h))
        for c, px, py, img in placed:
            tall.paste(img, (px, h * 2 + py))
        name = f'icons{n}_{w}x{h}.dds'
        write_dds_rgba(os.path.join(args.out, 'pad_icons', name), tall)
        tall.save(os.path.join(args.out, 'pad_icons', name[:-4] + '.png'))
        lines.append(f'atlas {hsh:016x} {w} {h} {total} {name}   # {a["source"]}')
        # (how a font's icon page is recognized: its Xbox glyphs' places)
        lines.append('xbox ' + ' '.join(f'{c:04x}:{tx[c][0]},{tx[c][1]},{tx[c][2]},{tx[c][3]}'
                                        for c in (0xE000, 0xE009, 0xE00B) if c in tx))
        for c in BUTTONS:
            if c in ty and c in tx:
                r = ty[c]
                lines.append(f'ps {c:04x} {r[0]} {r[1] + h} {r[2]} {r[3]}')
        for c, px, py, img in placed:
            lines.append(f'pair {c:04x} {px} {py + h * 2} {img.width} {img.height}')
        print(f'{name}: {a["source"]} ({len(placed)} pairs, {w}x{total})')
    with open(os.path.join(args.out, 'pad_icons', 'pad_icons.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
