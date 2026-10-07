"""practice_arena_build.py <game folder> <out folder>: the Practice Arena mod.

The training ring the game starts in (and MY WWE -> PRACTICE ARENA) is arena 29,
pac/bg/bg29.pac (stage STG/0029): a dark room with a ring, barricade and props,
no crowd, no stage. It plays any match as it is; the game renames its stage to
the slot's when a custom arena sits on another arena's tile (arena_mods.cpp
RedirectArena), so the file is used unchanged on base arena_slam (slot 4,
whose VS theme M04I is one 1024 x 512 background plus overlays).

Writes <out>/practice_arena/ (manifest, arena.pac, banner, load, vs/) and
<out>/practice_arena.svrmod (wwe13_pack.py, made_with=Port tools (SvR 2011 data)).
"""
import io
import os
import shutil
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wwe13_pack  # noqa: E402

FONT = r'C:\Windows\Fonts\impact.ttf'
MANIFEST = ('type=arena\nid=practice_arena\nname=Practice Arena\n'
            'author=SvR 2011 practice ring (arena 29), packed by port tools\nversion=1.0\nbase=arena_slam\n')


def dds(img, fourcc='DXT5'):
    b = io.BytesIO()
    img.convert('RGBA').save(b, 'DDS', pixel_format=fourcc)
    return b.getvalue()


def picture(w, h, title=True, sub=True):
    """Dark practice room: a soft ring light, two posts, three ropes, the title."""
    im = Image.new('RGB', (w, h), (0, 0, 0))
    glow = Image.new('L', (w, h), 0)
    ImageDraw.Draw(glow).ellipse((w * 0.15, h * 0.35, w * 0.85, h * 1.25), fill=70)
    glow = glow.filter(ImageFilter.GaussianBlur(w // 12))
    im = Image.composite(Image.new('RGB', (w, h), (120, 118, 110)), im, glow)
    d = ImageDraw.Draw(im)
    for yy, t in ((0.62, 0.012), (0.70, 0.010), (0.79, 0.009)):
        y = int(h * yy)
        d.line((0, y + int(h * 0.02), w, y - int(h * 0.02)), fill=(205, 200, 185), width=max(2, int(h * t)))
    for x in (int(w * 0.06), int(w * 0.94)):
        d.rectangle((x - int(w * 0.008), int(h * 0.55), x + int(w * 0.008), h), fill=(160, 160, 165))
    if title:
        size = int(h * 0.20)
        f = ImageFont.truetype(FONT, size)
        txt = 'PRACTICE ARENA'
        while d.textlength(txt, font=f) > w * 0.86:
            size -= 2
            f = ImageFont.truetype(FONT, size)
        tw = d.textlength(txt, font=f)
        x, y = (w - tw) / 2, h * 0.16
        d.text((x + 3, y + 3), txt, font=f, fill=(0, 0, 0))
        d.text((x, y), txt, font=f, fill=(235, 235, 235))
        if sub:
            f2 = ImageFont.truetype(FONT, max(10, size // 3))
            s2 = 'SMACKDOWN VS RAW 2011'
            d.text(((w - d.textlength(s2, font=f2)) / 2, y + size * 1.08), s2, font=f2, fill=(200, 40, 40))
    return im


def build(game, out):
    mod = os.path.join(out, 'practice_arena')
    os.makedirs(os.path.join(mod, 'vs'), exist_ok=True)
    src = os.path.join(game, 'pac', 'bg', 'bg29.pac')
    with open(src, 'rb') as f:
        head = f.read(0x810)
    if head[:4] != b'EPAC':
        raise SystemExit('%s: not an EPAC arena' % src)
    shutil.copyfile(src, os.path.join(mod, 'arena.pac'))
    with open(os.path.join(mod, 'manifest.txt'), 'w', newline='\n') as f:
        f.write(MANIFEST)
    # loading picture: shown stretched to 16:9, so its content is pre-squashed by 1/1.125
    load = Image.new('RGB', (1024, 512))
    load.paste(picture(1024, 512, title=False))
    tall = picture(1024, 512).resize((1024, round(512 / 1.125)))
    load.paste(tall, (0, (512 - tall.height) // 2))
    open(os.path.join(mod, 'load.dds'), 'wb').write(dds(load))
    open(os.path.join(mod, 'banner.dds'), 'wb').write(dds(picture(256, 128, sub=False)))
    # VS screen (SummerSlam theme M04I): its background, the dust and light overlays cleared
    open(os.path.join(mod, 'vs', 'BG.dds'), 'wb').write(dds(picture(1024, 512, sub=False)))
    for name, size in {'dust00': (16, 64), 'dust01': (256, 256), 'dust02': (64, 64), 'light00': (128, 128)}.items():
        open(os.path.join(mod, 'vs', name + '.dds'), 'wb').write(dds(Image.new('RGBA', size, (0, 0, 0, 0))))
    wwe13_pack.pack(mod, os.path.join(out, 'practice_arena.svrmod'), 'Port tools (SvR 2011 data)')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    build(sys.argv[1], sys.argv[2])
