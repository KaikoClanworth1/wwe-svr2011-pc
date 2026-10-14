"""Arenas from other games -> SvR 2011 arena mods (docs/MOD_MAKER_3_PLAN.md section 4).

    py tools/arena_port.py games
    py tools/arena_port.py list --game <id> --src <ISO or extract folder>
    py tools/arena_port.py convert --game <id> --src <...> --item <bgNN> --out <dir> [--base <0-19>] [--name <text>]

stdout is JSON only; logs and `progress <0-100> <text>` lines go to stderr.
Exit codes: 0 ok, 2 bad input, 3 conversion failed.

What each game needs (docs/OTHER_GAMES.md, "Arenas"):
- svr2010: the arena files are the same EPAC / JBOY / texture sets as 2011's;
  a 2010 arena plays in 2011 unchanged (tested: RAW bg01, 2026-10-13). The
  mod is the file as it is, plus a banner (2010's arena select banner,
  MatchHD MENU/MASI) and, where the base arena's VS theme has one 1024 x 512
  background, a VS and a loading picture from 2010's own VS theme (M<nn>I).
  The game gives the file its slot's stage name (arena_mods.cpp RedirectArena).
- wwe13: converted by the Mod Maker itself (svrfmt/wwe13.cpp); listed here
  as "native".
- svr2008: the same JBOY models, but the arena's texture sets are entries
  0xC8 / 0xC9, ids 2011 uses for models (the 2011 arena load crashed on them):
  they move to 0x190 / 0x191. 2008 has no crowd, announce table pictures,
  card or visibility tree in the arena file: the crowd (0x4E20-0x4E23,
  0x4E2A), 0x192, 0xEE48, 0xEA6C and the turnbuckle covers come from 2011 RAW
  (--pac11 <2011 game folder>). The arena keeps its own lights (0x7530), plus the
  light sets the game looks up itself that it lacks (2007: 11-14). Textures are halved (top mip dropped, biggest
  first) until the file is no bigger than that host's. Pictures: 2008's VS
  theme (M<nn>I) has its art; there is no select banner, so it is cut from it.
  Tested: RAW (bg01) plays in 2011 with the 2011 crowd.
- the rest: research (status "research"; convert exits 3).
"""
import argparse
import io
import json
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discfs  # noqa: E402
import svrfmt  # noqa: E402

GAMES = [
    {'id': 'svr2007', 'name': "WWE SmackDown vs. Raw 2007", 'platform': 'x360', 'disc_hint': '545107E0', 'status': 'ready'},
    {'id': 'svr2008', 'name': "WWE SmackDown vs. Raw 2008", 'platform': 'x360', 'disc_hint': '5451080B', 'status': 'ready'},
    {'id': 'svr2009', 'name': "WWE SmackDown vs. Raw 2009", 'platform': 'x360', 'disc_hint': '54510826', 'status': 'ready'},
    {'id': 'svr2010', 'name': "WWE SmackDown vs. Raw 2010", 'platform': 'x360', 'disc_hint': '54510844', 'status': 'ready'},
    {'id': 'wwe12', 'name': "WWE '12", 'platform': 'x360', 'disc_hint': '54510890', 'status': 'native'},
    {'id': 'wwe13', 'name': "WWE '13", 'platform': 'x360', 'disc_hint': '545108B4', 'status': 'native'},
    {'id': 'wwe2k14', 'name': "WWE 2K14", 'platform': 'x360', 'disc_hint': '545408B2', 'status': 'native'},
    # 2K15: plain EPAC + "ZLIB" entries as 2K14; 2K16: the same inside xcompress; 2K17: xcompressed 'HSPC'
    # with an 'SHDC' stage index (svrfmt/wwe13.cpp HspcStage13) - the same entries and model format
    {'id': 'wwe2k15', 'name': "WWE 2K15", 'platform': 'x360', 'disc_hint': '545408B6', 'status': 'native'},
    {'id': 'wwe2k16', 'name': "WWE 2K16", 'platform': 'x360', 'disc_hint': '315A07D2', 'status': 'native'},
    {'id': 'wwe2k17', 'name': "WWE 2K17", 'platform': 'x360', 'disc_hint': '545408BA', 'status': 'native'},
]
NATIVE_GAMES = ('wwe12', 'wwe13', 'wwe2k14', 'wwe2k15', 'wwe2k16', 'wwe2k17')

# Arenas that convert but crash 2011 at load (full pass, docs/ARENA_PORT_RESULTS.md): 'list' marks
# them status 'crashes' with the reason, so the Mod Maker can grey them out
# (not listed: WWE 2K14 bg44-54 crashed once in the crowd renderer (lr 8217FF84) but played 5 of 5
# repeat runs - bg51 x3, bg44 x2 - and bg55 / 56 likewise: an intermittent crash, open)
KNOWN_CRASHES = {
    # (only arenas that crashed on every repeat run)
    ('wwe13', 'bg37'): 'crashes at load (a memory allocation fails; not its size or models) - 3 of 3 runs, open',
    ('wwe2k14', 'bg00'): 'crashes at load - 2 of 2 runs, open',
    ('wwe2k14', 'bg12'): 'crashes at load (object motion loader, lr 826AEF18) - 3 of 3 runs, open',
    ('wwe2k14', 'bg17'): 'crashes at load (lr 82188A00) - 3 of 3 runs, open',
}


def mark_items(game, items):
    """'list' items get status: 'ok', 'crashes' (KNOWN_CRASHES) or 'special' (WWE '12+ bg60 and up:
    backstage rooms, studio / photo stages - no arenas), and a reason for the last two"""
    for it in items:
        key = it.get('key', '')
        if (game, key) in KNOWN_CRASHES:
            it['status'], it['reason'] = 'crashes', KNOWN_CRASHES[(game, key)]
        elif game in NATIVE_GAMES and key[:2] == 'bg' and key[2:4].isdigit() and int(key[2:4]) >= 60:
            it['status'], it['reason'] = 'special', 'a backstage room or studio / photo stage, not an arena'
        else:
            it.setdefault('status', 'ok')
    return items

# 2011's arena select tiles: arena number -> banner / manifest base name (arena_mods.cpp kTiles)
BASE_NAMES = {0: 'arena_SD', 1: 'arena_RAW', 2: 'arena_ECW', 3: 'arena_ttt', 4: 'arena_slam', 5: 'arena_BP',
              6: 'arena_HinC', 7: 'arena_Series', 8: 'arena_TLC', 9: 'arena_RR', 10: 'arena_EC', 11: 'arena_WM26',
              12: 'arena_back', 13: 'arena_Judg', 14: 'arena_NoC', 15: 'arena_BASH', 16: 'arena_EXT', 17: 'arena_SS',
              18: 'arena_BR', 19: 'arena_drui'}
# a base theme's one 1024 x 512 VS background (MatchHD MENU/M<nn>I), where it has one
VS_BACKGROUND = {0: 'bg_sd', 1: 'RAW01', 3: 'troops_tex05', 4: 'BG', 11: 'wm_bg01'}

# SvR 2010: arena number -> (select banner name in MASI, show, 2011 arena of that show)
# (from each bgNN's own texture names; bg16 / bg20 / bg22 / bg25 are story copies)
SVR2010 = {
    0: ('arena_SD', 'SmackDown', 0), 1: ('arena_raw', 'RAW', 1), 2: ('arena_ecw', 'ECW', 2),
    4: ('arena_SS', 'SummerSlam', 4), 5: ('arena_UG', 'Unforgiven', None), 6: ('arena_no', 'No Mercy', None),
    7: ('arena_sur', 'Survivor Series', 7), 8: ('arena_arm', 'Armageddon', None), 9: ('arena_RR', 'Royal Rumble', 9),
    10: ('arena_out', 'No Way Out', None), 11: ('arena_WM25', 'WrestleMania 25', None), 12: ('arena_back', 'Backlash', None),
    13: ('arena_judg', 'Judgment Day', 13), 14: ('arena_NoC', 'Night of Champions', 14),
    15: ('arena_GAB', 'The Great American Bash', 15), 18: ('arena_CS', 'Cyber Sunday', None),
    24: (None, 'Extreme Rules', 16),
}
# SvR 2008: arena number -> (show, 2011 arena of that show); bg20-27 are story copies
SVR2008 = {
    0: ('SmackDown', 0), 1: ('RAW', 1), 2: ('ECW', 2), 3: ('Sunday Night Heat', None), 4: ('SummerSlam', 4),
    5: ('Unforgiven', None), 6: ('No Mercy', None), 7: ('Survivor Series', 7), 8: ('Armageddon', None),
    9: ('Royal Rumble', 9), 10: ('No Way Out', None), 11: ('WrestleMania 23', None), 12: ('Backlash', None),
    13: ('Judgment Day', 13), 14: ('Vengeance', None), 15: ('The Great American Bash', 15),
    16: ("New Year's Revolution", None), 17: ('One Night Stand', None), 18: ('December to Dismember', None),
    19: ('Cyber Sunday', None),
}
# SvR 2009: arena number -> (show, 2011 arena of that show); the same layout as 2008's
# (its own crowd and 0x7922 are an older format: replaced by the host's / left out)
SVR2009 = {
    0: ('SmackDown', 0), 1: ('RAW', 1), 2: ('ECW', 2), 3: ('Tribute to the Troops', 3), 4: ('SummerSlam', 4),
    5: ('Unforgiven', None), 6: ('No Mercy', None), 7: ('Survivor Series', 7), 8: ('Armageddon', None),
    9: ('Royal Rumble', 9), 10: ('No Way Out', None), 11: ('WrestleMania XXIV', None), 12: ('Backlash', None),
    13: ('Judgment Day', 13), 14: ('Night of Champions', 14), 15: ('The Great American Bash', 15),
    16: ('One Night Stand', None), 17: ('Cyber Sunday', None), 18: ('Vengeance', None),
}
# SvR 2007: stage number -> (show, 2011 arena of that show). Its arenas are
# entries STG<k>/00NN of pac/bg/bg<k>.pac (k = NN // 10); 0020+ are copies.
SVR2007 = {
    0: ('SmackDown', 0), 1: ('RAW', 1), 2: ('ECW', 2), 4: ('SummerSlam', 4), 5: ('Unforgiven', None),
    6: ('No Mercy', None), 7: ('Survivor Series', 7), 8: ('Armageddon', None), 9: ('Royal Rumble', 9),
    10: ('No Way Out', None), 11: ('WrestleMania 22', None), 12: ('Backlash', None), 13: ('Judgment Day', 13),
    14: ('Vengeance', None), 15: ('The Great American Bash', 15), 16: ("New Year's Revolution", None),
    17: ('One Night Stand', None), 18: ('Cyber Sunday', None), 19: ("Saturday Night's Main Event", None),
}
HOST_ENTRIES = (0x192, 0x4E20, 0x4E21, 0x4E22, 0x4E23, 0x4E2A, 0xEE48, 0xEA6C)
# SvR 2007-2009 turnbuckle covers (ar_cover10-13): a black logo pad (rin_tb00) inside a white taped
# shell (rin_tb10) that 2011 draws over the pad - the "white turnbuckles". 2011 RAW's covers are the
# same black logo pads with no shell, on the same ring.
HOST_COVERS = (0x3C4, 0x3C5, 0x3C6, 0x3C7)
MAX_ARENA = 9_400_000  # bytes: the smaller 2011 hosts (a bigger file can run out of the arena heap)


class BadInput(Exception):
    pass


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def progress(p, text):
    print('progress %d %s' % (p, text), file=sys.stderr, flush=True)


def textures(raw):
    """A texture set (u32 count, 32-byte records name[16] 'dds' size offset) -> [(name, dds bytes)]."""
    n = struct.unpack_from('<I', raw, 0)[0]
    if not (0 < n < 600) or 16 + 32 * n > len(raw) or raw[32:35] != b'dds':
        return []
    out = []
    for i in range(n):
        r = raw[16 + 32 * i:48 + 32 * i]
        size, off = struct.unpack_from('<II', r, 20)
        out.append((r[:16].split(b'\0')[0].decode('latin-1'), raw[off:off + size]))
    return out


def group_textures(pac_bytes, entry_name):
    """Every texture of an EPAC entry (MENU/<entry_name>), from its PACH children."""
    _, groups, _ = svrfmt.epac_read(pac_bytes)
    out = []
    for _, ents in groups:
        for nm, blob in ents:
            if nm.split(b'\0')[0].decode('latin-1') != entry_name or blob[:4] != b'PACH':
                continue
            for _, child in svrfmt.pach_read(blob):
                raw = svrfmt.bpe_decode(child) if child[:4] == b'BPE ' else child
                out += textures(raw)
    return out


def image(dds):
    from PIL import Image
    return Image.open(io.BytesIO(dds)).convert('RGBA')


def dds_dxt5(img):
    b = io.BytesIO()
    img.convert('RGBA').save(b, 'DDS', pixel_format='DXT5')
    return b.getvalue()


def fit(img, w, h):
    """img scaled to cover w x h, centred and cropped."""
    from PIL import Image
    s = max(w / img.width, h / img.height)
    im = img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.LANCZOS)
    x, y = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((x, y, x + w, y + h))


def banner_tile(banner, art):
    """A 256 x 128 tile: a wide banner fills it; a square one (2010's PPVs) is
    shown whole, over its VS picture darkened (or black)."""
    from PIL import Image, ImageEnhance
    if banner.width >= 1.6 * banner.height:
        return fit(banner, 256, 128)
    bg = fit(art, 256, 128) if art is not None else Image.new('RGBA', (256, 128), (0, 0, 0, 255))
    bg = ImageEnhance.Brightness(bg.convert('RGB')).enhance(0.45).convert('RGBA')
    s = 128 / banner.height
    b = banner.resize((max(1, round(banner.width * s)), 128), Image.LANCZOS)
    bg.paste(b, ((256 - b.width) // 2, 0), b)
    return bg


# -- SvR 2010 ------------------------------------------------------------------

def svr2010_list(disc):
    items = []
    for n, (banner, show, in11) in sorted(SVR2010.items()):
        path = 'pac/bg/bg%02d.pac' % n
        if not disc.exists(path):
            continue
        items.append({'key': 'bg%02d' % n, 'name': show, 'arena': n, 'size': disc.size(path),
                      'banner': banner, 'in_2011': in11})
    return items


def svr2010_convert(disc, key, out, base=None, name=None):
    warnings, notes, dropped = [], [], []
    try:
        n = int(key[2:]) if key.lower().startswith('bg') else int(key)
    except ValueError:
        raise BadInput('item %r: expected bgNN' % key)
    if n not in SVR2010:
        raise BadInput('item %r: not an arena of SvR 2010 (story copies are left out)' % key)
    banner_name, show, in11 = SVR2010[n]
    path = 'pac/bg/bg%02d.pac' % n
    if not disc.exists(path):
        raise BadInput('%s not on the disc' % path)
    if base is None:
        base = in11 if in11 is not None else (n if n in BASE_NAMES else 3)
    if base not in BASE_NAMES:
        raise BadInput('base %r: 0-19' % base)
    name = name or '%s (SvR 2010)' % show
    mod_id = 'svr2010_' + ''.join(c if c.isalnum() else '_' for c in show.lower()).strip('_')

    progress(5, 'reading %s' % path)
    arena = disc.read(path)
    if arena[:4] != b'EPAC':
        raise RuntimeError('%s: not an EPAC arena' % path)
    if len(arena) > MAX_ARENA:
        warnings.append('the arena file is %d bytes; 2011 hosts hold about 9.0-9.6 MB' % len(arena))
    mod = os.path.join(out, mod_id)
    os.makedirs(os.path.join(mod, 'vs'), exist_ok=True)
    with open(os.path.join(mod, 'arena.pac'), 'wb') as f:
        f.write(arena)

    progress(40, 'pictures')
    match = disc.read('pac/menu/MatchHD.pac') if disc.exists('pac/menu/MatchHD.pac') else None
    banner = None
    if match and banner_name:
        for nm, dds in group_textures(match, 'MASI'):
            if nm == banner_name:
                banner = image(dds)
    art = None  # the 2010 VS theme's biggest picture
    if match:
        theme = group_textures(match, 'M%02dI' % n)
        best = 0
        for nm, dds in theme:
            try:
                im = image(dds)
            except Exception:
                continue
            if im.width * im.height > best and im.width >= 256:
                art, best = im, im.width * im.height
    if banner is None and art is not None:
        banner = art
        notes.append('no select banner on the disc for this arena: the banner is cut from its VS picture')
    from PIL import Image
    if banner is None:
        banner = Image.new('RGBA', (256, 128), (30, 30, 30, 255))
        warnings.append('no banner or VS picture found: a plain banner was made')
    with open(os.path.join(mod, 'banner.dds'), 'wb') as f:
        f.write(dds_dxt5(banner_tile(banner, art)))
    if art is not None:
        with open(os.path.join(mod, 'load.dds'), 'wb') as f:
            f.write(dds_dxt5(fit(art, 1024, 512)))
        if base in VS_BACKGROUND:
            with open(os.path.join(mod, 'vs', VS_BACKGROUND[base] + '.dds'), 'wb') as f:
                f.write(dds_dxt5(fit(art, 1024, 512)))
        else:
            notes.append("the VS screen keeps %s's theme (it has no single 1024 x 512 background)" % BASE_NAMES[base])
    else:
        notes.append('no loading or VS picture (no VS theme for this arena on the disc)')
    if not os.listdir(os.path.join(mod, 'vs')):
        os.rmdir(os.path.join(mod, 'vs'))

    with open(os.path.join(mod, 'manifest.txt'), 'w', newline='\n') as f:
        f.write('type=arena\nid=%s\nname=%s\nauthor=SvR 2010 arena %s, ported by port tools\nversion=1.0\nbase=%s\n'
                % (mod_id, name, 'bg%02d' % n, BASE_NAMES[base]))
    progress(80, 'packing')
    import wwe13_pack
    svrmod = os.path.join(out, mod_id + '.svrmod')
    old = sys.stdout
    sys.stdout = sys.stderr  # (the packer prints a line)
    try:
        wwe13_pack.pack(mod, svrmod, 'Port tools (SvR 2010)')
    finally:
        sys.stdout = old
    notes.append('base %s (tile %d on the custom arena pages); the game renames its stage to the slot\'s' % (BASE_NAMES[base], base))
    if n not in (0, 1, 2, 4, 7, 9, 13, 14, 15) and in11 is None:
        notes.append('2011 has no %s arena: the mod sits on the %s tile' % (show, BASE_NAMES[base]))
    progress(100, 'done')
    return {'ok': True, 'out': svrmod, 'folder': mod, 'opens': 'arena', 'warnings': warnings, 'dropped': dropped,
            'notes': notes}


def drop_top_mip(dds):
    """A DDS one size down: its top level removed (the next mip becomes the top)."""
    h, w = struct.unpack_from('<II', dds, 12)
    mips = struct.unpack_from('<I', dds, 28)[0]
    four = dds[84:88]
    if mips < 2 or four not in (b'DXT1', b'DXT3', b'DXT5') or w < 8 or h < 8:
        return None
    block = 8 if four == b'DXT1' else 16
    top = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block
    out = bytearray(dds[:128])
    struct.pack_into('<II', out, 12, h // 2, w // 2)
    struct.pack_into('<I', out, 28, mips - 1)
    if struct.unpack_from('<I', out, 20)[0]:  # pitch / linear size
        struct.pack_into('<I', out, 20, top // 4)
    return bytes(out) + dds[128 + top:]


def texture_set_write(texs, head):
    n = len(texs)
    off = (16 + 32 * n + 15) & ~15
    recs, blobs = b'', b''
    for name, dds in texs:
        recs += (name.encode('latin-1')[:15].ljust(16, b'\0') + b'dds\0'
                 + struct.pack('<II', len(dds), off + len(blobs)) + bytes(4))
        blobs += dds + bytes(-len(dds) % 16)
    return (struct.pack('<I', n) + head[4:16] + recs).ljust(off, b'\0') + blobs


def find_host(pac11, base):
    for p in (os.path.join(pac11, 'pac', 'bg', 'bg%02d.pac' % base), os.path.join(pac11, 'bg%02d.pac' % base),
              os.path.join(pac11, 'bg', 'bg%02d.pac' % base)):
        if os.path.exists(p):
            return p
    raise BadInput('--pac11 %s: no bg%02d.pac (give the SvR 2011 game folder)' % (pac11, base))


# Arena lights: entry 0x7530 of each arena is a YMKs bank of light sets (ids 10-14, 20, 30, 100-117,
# 200-202, 300-306, 450-454, 500-503 in 2011 RAW). Each set: ground / sky colour (y 0 / 1), up to 4
# directional lights (y 10-13) and moving lights (y 50-59), as 25-byte keys (position, rotation,
# colour). The model flags (0xC351) pick a set per model with l(n), the light zones (0xC353) per
# area of the floor (entrance stage, ramp); a model without l() gets set 100. Every SvR 2007-2010
# and WWE '12-2K14 arena has its own bank in this format, with the same id plan.
EXE_LIGHTS = (10, 11, 12, 13, 14, 20, 30, 31, 100, 110, 115, 500)  # sets the game looks up itself


def light_bank(raw):
    """0x7530 bytes (2007-2009: no YMKs header, which 2011 accepts) -> ymks.Bank"""
    import ymks
    if raw[:4] not in (b'YMKs', b'YMBs'):
        raw = b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + raw
    return ymks.read_bank(raw)


def light_ids(bank):
    return sorted({e['id'] for e in bank.entries})


def fill_lights(bank, host_bank, ids=EXE_LIGHTS, replace=()):
    """Light sets the game looks up by number that the arena's bank lacks (SvR 2007 has no 11-14;
    its own bank crashed the 2011 load without them) are copied from 2011 RAW's -> [ids added].
    Sets in `replace` are RAW's even when the arena has them."""
    if replace:
        bank.entries = [e for e in bank.entries if e['id'] not in replace]
    have = set(light_ids(bank))
    added = []
    for i in list(ids) + [r for r in replace if r not in ids]:
        if i in have:
            continue
        ents = [dict(e) for e in host_bank.entries if e['id'] == i]
        if ents:
            ents[0]['share'] = False
            bank.entries += ents
            added.append(i)
    return added


def remap_lights(flags, have):
    """Model flag text (0xC351): l(n) / le(n) / lg(n) sets missing from the light bank the arena
    ships (have: its ids) go to the nearest set of the same hundred (an unknown set is index -1
    in the game)."""
    import re
    have = sorted(have)
    missing = {}

    def sub(m):
        n = int(m.group(2))
        if n in have or not have:
            return m.group(0)
        same = [h for h in have if h // 100 == n // 100] or have
        to = min(same, key=lambda h: abs(h - n))
        missing[n] = to
        return m.group(1) + b'(%d)' % to
    return re.sub(rb'\b(l[eg]?)\((\d+)\)', sub, flags), missing


def vis_tree(ents):
    """A visibility tree (entry 0xC364) for an arena that has none: SvR2011 culls every arena
    model its tree doesn't list (2007-2009 arenas came out with a black floor and no barricades).
    One node per arena model (ids below 0x190), boxed by its vertices: 3ds Max axes in metres,
    (x, z, -y) x 0.1 of the model's units, as 2011's own trees."""
    import jboy
    boxes = []
    for i, x in sorted(ents.items()):
        if i >= 0x190:
            continue
        d = svrfmt.bpe_decode(x) if x[:4] == b'BPE ' else x
        if d[:4] != b'JBOY':
            continue
        m = jboy.read(d)
        v = [p for msh in m.meshes for p in msh.verts]
        if not v:
            continue
        xs, ys, zs = [p[0] * 0.1 for p in v], [p[2] * 0.1 for p in v], [-p[1] * 0.1 for p in v]
        boxes.append((m.name, (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))))
    if not boxes:
        return None
    root = [min(b[1][k] for b in boxes) for k in range(3)] + [max(b[1][k] for b in boxes) for k in range(3, 6)]

    def f(b):
        return ' '.join('%g' % round(c, 5) for c in b)
    out = [';<\u30a2\u30ea\u30fc\u30ca\u30e2\u30c7\u30eb\u5206\u5272\u306b\u3088\u308b\u53ef\u8996\u5224\u5b9a'
           '\u30c7\u30fc\u30bf\u30d5\u30a1\u30a4\u30eb>', ';\u5ea7\u6a19\u7cfb=3ds max', '',
           'NODE MY_NAME = VT_ROOT', '{', '\t' + f(root), '}', str(len(boxes)), '']
    for name, b in boxes:
        out += ['NODE MY_NAME = %s PARENT_NAME = VT_ROOT' % name, '{', '\t' + f(b), '}', '0', '']
    return '\r\n'.join(out).encode('cp932')


def svr2007_path(n):
    return 'pac/bg/bg%d.pac' % (n // 10)


def svr2008_list(disc, table=None):
    items = []
    for n, (show, in11) in sorted((table or SVR2008).items()):
        path = svr2007_path(n) if table is SVR2007 else 'pac/bg/bg%02d.pac' % n
        if disc.exists(path):
            items.append({'key': 'bg%02d' % n, 'name': show, 'arena': n, 'size': disc.size(path), 'in_2011': in11})
    return items


def svr2008_convert(disc, key, out, pac11, base=None, name=None, game='svr2008'):
    table = {'svr2009': SVR2009, 'svr2007': SVR2007}.get(game, SVR2008)
    year = game[3:]
    warnings, notes, dropped = [], [], []
    try:
        n = int(key[2:]) if key.lower().startswith('bg') else int(key)
    except ValueError:
        raise BadInput('item %r: expected bgNN' % key)
    if n not in table:
        raise BadInput('item %r: not an arena of SvR %s (story copies are left out)' % (key, year))
    if not pac11:
        raise BadInput('SvR 2008 / 2009 arenas need --pac11 <SvR 2011 game folder> (crowd and table pictures)')
    show, in11 = table[n]
    if base is None:
        base = in11 if in11 is not None else (n if n in BASE_NAMES else 3)
    if base not in BASE_NAMES:
        raise BadInput('base %r: 0-19' % base)
    name = name or '%s (SvR %s)' % (show, year)
    mod_id = game + '_' + ''.join(c if c.isalnum() else '_' for c in show.lower()).strip('_')
    progress(3, 'reading bg%02d' % n)
    # the file's layout and size budget: the base slot's arena; the borrowed entries (crowd,
    # table pictures, card, light set): always 2011's RAW (a 2008 arena with SummerSlam's
    # crowd / lights crashed the load on its slot; with RAW's it plays there)
    host = open(find_host(pac11, base), 'rb').read()
    hb, gb, tb = svrfmt.epac_read(host)
    _, graw, _ = svrfmt.epac_read(open(find_host(pac11, 1), 'rb').read())
    eb = dict(svrfmt.pach_read(graw[0][1][0][1]))
    if game == 'svr2007':  # one stage of a many-stage pac
        _, ga, _ = svrfmt.epac_read(disc.read(svr2007_path(n)))
        stage = next((b for typ, es in ga for nm, b in es if typ.startswith(b'STG') and nm == b'%04d' % n), None)
        if stage is None:
            raise BadInput('%s has no stage %04d' % (svr2007_path(n), n))
    else:
        _, ga, _ = svrfmt.epac_read(disc.read('pac/bg/bg%02d.pac' % n))
        stage = ga[0][1][0][1]
    ents = {}
    for i, x in svrfmt.pach_read(stage):
        # (2007: its crowd is loose models and seat files 0x4E20-0x52FF, plus 0x9C40-0x9CFF)
        if game == 'svr2007' and (0x4E20 <= i <= 0x52FF or 0x9C40 <= i <= 0x9CFF):
            continue
        ents[{0xC8: 0x190, 0xC9: 0x191}.get(i, i)] = x
    if 0x190 not in ents:
        raise RuntimeError('bg%02d: no texture set (0xC8)' % n)
    for i in (0x4E20, 0x4E21, 0x4E22, 0x4E23, 0x4E2A, 0x7922):  # older crowd / motion formats
        ents.pop(i, None)
    for i in HOST_ENTRIES + HOST_COVERS:
        if i in eb:
            ents[i] = eb[i]
    unp = lambda b: svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b  # noqa: E731
    have = None
    if 0x7530 in ents and 0x7530 in eb:  # the arena's own lights, plus the sets the game needs
        import ymks
        bank = light_bank(unp(ents[0x7530]))
        # (set 10, the game's own light for the wrestlers and the ring, is the same in every arena
        # of a game; SvR 2008's side lights run at 1.2-3.0x - the others' at most ~1.5x, 2011's
        # 0.6x - and blew the black turnbuckle pads out to cream in 2011. With SvR 2008's or 2009's
        # set 10 some arenas crashed ~10 s into a match in the code that reads it (sub_822A6BD8,
        # lr 822A6EE8: 2008 bg04 / bg06 / bg07, 2009 bg09 / bg17, run to run): RAW's)
        added = fill_lights(bank, light_bank(unp(eb[0x7530])), replace=(10,) if game in ('svr2008', 'svr2009') else ())
        if added:
            ents[0x7530] = ymks.write_bank(bank)
            notes.append('light sets the game needs, from 2011 RAW: ' + ', '.join(map(str, added)))
        have = light_ids(bank)
    if 0xC364 not in ents:
        tree = vis_tree(ents)
        if tree:
            ents[0xC364] = svrfmt.bpe_encode(tree, compress=True)
            notes.append('visibility tree made from the arena models (the game had none)')
    if 0xC351 in ents and have:
        flags, moved = remap_lights(unp(ents[0xC351]), have)
        if moved:
            ents[0xC351] = svrfmt.bpe_encode(flags, compress=True)
            notes.append('light sets missing from the arena, moved: ' + ', '.join('%d -> %d' % kv for kv in sorted(moved.items())))
    notes.append('crowd, announce table pictures and card from 2011 RAW')

    def build(tex_blob):
        e = dict(ents)
        e[0x190] = tex_blob
        g = [(gb[0][0], [(gb[0][1][0][0], svrfmt.pach_write(sorted(e.items())))])] + list(gb[1:])
        return svrfmt.epac_write(hb, g, tb)

    budget = len(host)
    data = build(ents[0x190])
    raw = svrfmt.bpe_decode(ents[0x190])
    texs = textures(raw)
    head = raw[:16]
    halved = set()
    rnd = 0
    while len(data) > budget:
        rnd += 1
        if rnd > 6:
            raise RuntimeError('cannot fit bg%02d into %d bytes' % (n, budget))
        over = len(data) - budget
        saved = 0
        for k in sorted(range(len(texs)), key=lambda k: -len(texs[k][1])):
            if saved * 0.67 > over * 1.15:
                break
            smaller = drop_top_mip(texs[k][1])
            if smaller is None:
                continue
            saved += len(texs[k][1]) - len(smaller)
            texs[k] = (texs[k][0], smaller)
            halved.add(texs[k][0])
        progress(10 + rnd * 12, 'packing textures, round %d' % rnd)
        data = build(svrfmt.bpe_encode(texture_set_write(texs, head), compress=True))
        log('round %d: file %d, budget %d' % (rnd, len(data), budget))
    if halved:
        notes.append('%d textures at half size to fit the %s file (%d bytes)' % (len(halved), BASE_NAMES[base], budget))

    mod = os.path.join(out, mod_id)
    os.makedirs(os.path.join(mod, 'vs'), exist_ok=True)
    with open(os.path.join(mod, 'arena.pac'), 'wb') as f:
        f.write(data)
    progress(85, 'pictures')
    art = None
    if disc.exists('pac/menu/MatchHD.pac'):
        best = 0
        for nm, dds in group_textures(disc.read('pac/menu/MatchHD.pac'), 'M%02dI' % n):
            try:
                im = image(dds)
            except Exception:
                continue
            if im.width >= 256 and im.width * im.height > best:
                art, best = im, im.width * im.height
    from PIL import Image
    if art is None:
        warnings.append('no VS picture for this arena on the disc: a plain banner was made')
    banner = art if art is not None else Image.new('RGBA', (256, 128), (30, 30, 30, 255))
    with open(os.path.join(mod, 'banner.dds'), 'wb') as f:
        f.write(dds_dxt5(fit(banner, 256, 128)))
    if art is not None:
        with open(os.path.join(mod, 'load.dds'), 'wb') as f:
            f.write(dds_dxt5(fit(art, 1024, 512)))
        if base in VS_BACKGROUND:
            with open(os.path.join(mod, 'vs', VS_BACKGROUND[base] + '.dds'), 'wb') as f:
                f.write(dds_dxt5(fit(art, 1024, 512)))
    if not os.listdir(os.path.join(mod, 'vs')):
        os.rmdir(os.path.join(mod, 'vs'))
    with open(os.path.join(mod, 'manifest.txt'), 'w', newline='\n') as f:
        f.write('type=arena\nid=%s\nname=%s\nauthor=SvR %s arena bg%02d, ported by port tools\nversion=1.0\nbase=%s\n'
                % (mod_id, name, year, n, BASE_NAMES[base]))
    progress(92, 'packing the mod')
    import wwe13_pack
    svrmod = os.path.join(out, mod_id + '.svrmod')
    old = sys.stdout
    sys.stdout = sys.stderr
    try:
        wwe13_pack.pack(mod, svrmod, 'Port tools (SvR %s)' % year)
    finally:
        sys.stdout = old
    if in11 is None:
        notes.append('2011 has no %s arena: the mod sits on the %s tile' % (show, BASE_NAMES[base]))
    progress(100, 'done')
    return {'ok': True, 'out': svrmod, 'folder': mod, 'opens': 'arena', 'warnings': warnings, 'dropped': dropped,
            'notes': notes}


def bg_list(disc):
    """Arena files of a game not yet supported (for the research table)."""
    items = []
    for nm in sorted(disc.listdir('pac/bg')):
        low = nm.lower()
        if low.startswith('bg') and low.endswith('.pac') and low[2:4].isdigit():
            items.append({'key': low[:-4], 'name': low[:-4], 'arena': int(low[2:4]), 'size': disc.size('pac/bg/' + nm),
                          'in_2011': None})
    return items


def main(argv):
    ap = argparse.ArgumentParser(prog='arena_port.py')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('games')
    p = sub.add_parser('list')
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--pac11')
    p = sub.add_parser('convert')
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--item', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--base', type=int)
    p.add_argument('--name')
    p.add_argument('--pac11')
    try:
        a = ap.parse_args(argv)
    except SystemExit:
        print(json.dumps({'ok': False, 'error': 'bad arguments'}))
        return 2
    if a.cmd == 'games':
        print(json.dumps(GAMES))
        return 0
    game = next((g for g in GAMES if g['id'] == a.game), None)
    try:
        if not game:
            raise BadInput('unknown game %r' % a.game)
        if not os.path.exists(a.src):
            raise BadInput('--src %s: not found' % a.src)
        with discfs.open(a.src) as disc:
            if not disc.isdir('pac/bg'):
                raise BadInput('the game files have no pac/bg folder (an incomplete extract? use the whole disc '
                               'folder or the ISO)')
            if a.cmd == 'list':
                lists = {'svr2010': svr2010_list, 'svr2008': svr2008_list,
                         'svr2009': lambda d: svr2008_list(d, SVR2009),
                         'svr2007': lambda d: svr2008_list(d, SVR2007)}
                print(json.dumps(mark_items(a.game, lists.get(a.game, bg_list)(disc))))
                return 0
            if a.game not in ('svr2010', 'svr2008', 'svr2009', 'svr2007'):
                msg = ("WWE '12 - 2K17 arenas are converted by the Mod Maker itself (svrmod wwe13)" if a.game in NATIVE_GAMES
                       else '%s arenas are not convertible yet (research)' % game['name'])
                rep = {'ok': False, 'out': None, 'opens': 'arena', 'warnings': [], 'dropped': [], 'notes': [msg]}
                print(json.dumps(rep))
                return 3
            os.makedirs(a.out, exist_ok=True)
            if a.game in ('svr2007', 'svr2008', 'svr2009'):
                rep = svr2008_convert(disc, a.item, a.out, a.pac11, a.base, a.name, a.game)
            else:
                rep = svr2010_convert(disc, a.item, a.out, a.base, a.name)
            with open(os.path.join(a.out, 'report.json'), 'w') as f:
                json.dump(rep, f, indent=1)
            print(json.dumps(rep))
            return 0
    except BadInput as e:
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 2
    except Exception as e:  # noqa: BLE001 - reported to the Mod Maker
        log('failed: %r' % e)
        print(json.dumps({'ok': False, 'error': str(e), 'out': None, 'opens': 'arena', 'warnings': [], 'dropped': [],
                          'notes': []}))
        return 3


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
