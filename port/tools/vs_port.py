"""VS screen styles from older SmackDown vs. Raw games (SvR 2008 / 2009 / 2010).

The VS screen (the match set-up page: both renders, "A VS. B", the menu row and
"<show> TONIGHT") is drawn over a per-arena VS theme, three entries of
pac/menu/MatchHD.pac in 2011: M<nn>L (the YXML layout), M<nn>I (its textures
and ATB animations) and M<nn>F (its font). 2008 - 2010 have the same system:
the same YXMLBIN VER0002 layouts with the same layer and text names, only
their M<nn>L are in pac/menu/menu.pac. Their themes play in 2011 as they are;
only their text lines sit lower than 2011's menu row allows, so each text
node is given the place, size and width of 2011's node of that name in the
same theme (docs/VS_STYLES.md).

This makes a VS style from the player's own disc - never shipped:
<out>/manifest.txt (type=VsStyle) and vs.pac (an EPAC, group MENU, the
themes under 2011's numbers); installed as <game>/Mods/VsStyles/<game id>/.
The game merges the installed styles into an overlay and shows the chosen
one (GRAPHICS -> VS SCREEN STYLE).

The Mod Maker's converter contract (docs/MOD_MAKER_3_PLAN.md section 4):
    py vs_port.py games
    py vs_port.py list --game svr2010 --src <disc image or folder>     (one item: the style)
    py vs_port.py convert --game svr2010 --src <disc> --pac11 <2011 game or its pac folder> --out <folder>
        [--item style] [--renders]  (--renders: the old game's renders for 2011's superstars, matched by
                                     name, as a media pack beside it - vs_renders.py)

JSON on stdout (and <out>/report.json). Exit codes: 0 ok, 2 bad input, 3 conversion failed.
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discfs  # noqa: E402
import svrfmt as f  # noqa: E402
from arena_port import SVR2008, SVR2009, SVR2010  # noqa: E402

GAMES = {'svr2008': 'SvR 2008', 'svr2009': 'SvR 2009', 'svr2010': 'SvR 2010'}
DISC = {'svr2008': '5451080B', 'svr2009': '54510826', 'svr2010': '54510844'}  # (the Xbox 360 title ids: disc_hint)
# Rebuilt into 2011's shape (reshape) their themes no longer crash 2011 and the superstars show, but the theme's own
# pictures and the names line don't draw yet (2011's VS screen code: being looked into).
RESEARCH = {'svr2008': "SvR 2008's VS screens don't draw in 2011 yet - not ready yet",
            'svr2009': "SvR 2009's VS screens don't draw in 2011 yet - not ready yet"}


class BadInput(Exception):
    pass


def shows(game):
    """old theme number -> (show, 2011 theme number or None)"""
    if game == 'svr2010':
        return {n: (v[1], v[2]) for n, v in SVR2010.items()}
    return dict(SVR2008 if game == 'svr2008' else SVR2009)


def theme_map(game):
    """2011 theme number -> old theme number: the arena tables' "2011 arena of that show", plus the shows 2011
    has a theme for under another event's name (WrestleMania 11, Backlash 12)."""
    out = {}
    for n, (show, n11) in sorted(shows(game).items()):
        if n11 is None:
            n11 = 11 if 'WrestleMania' in show else 12 if show == 'Backlash' else None
        if n11 is not None and n11 not in out:
            out[n11] = n
    return out


def entries(data):
    h, groups, t = f.epac_read(data)
    return h, {n.decode(errors='replace').strip('\0'): b for typ, ents in groups for n, b in ents}


def unwrap(blob):
    return f.bpe_decode(blob) if blob[:4] == b'BPE ' else blob


# ---------------------------------------------------------------- the layouts' text nodes

TEXT = re.compile(rb'font000_[a-z]+')


def text_nodes(y):
    """[(name, x/y/scale/width offset)] in file order: font id, x, y, scale x, scale y, width (6 x 4 bytes) sit
    0x5C bytes before the node's name."""
    return [(m.group().decode(), m.start() - 0x58) for m in TEXT.finditer(y)]


def move_text(old, new11, fallback=()):
    """The old layout with each text node at 2011's place (x, y, scale, width) for the node of that name and
    occurrence in the same theme (else in `fallback`, 2011's SmackDown / RAW layouts); a node no 2011 layout
    has (2008's main / main2 / sub2: the game never fills them) scaled to nothing."""
    out = bytearray(old)
    ref = {}
    for y11 in (new11,) + tuple(fallback):
        own = {}
        for name, at in text_nodes(y11):
            own.setdefault(name, []).append(struct.unpack_from('<5f', y11, at))
        for name, v in own.items():
            ref.setdefault(name, v)
    seen = {}
    moved = hidden = 0
    for name, at in text_nodes(old):
        k = seen.get(name, 0)
        seen[name] = k + 1
        r = ref.get(name)
        if r:
            struct.pack_into('<5f', out, at, *r[min(k, len(r) - 1)])
            moved += 1
        else:
            x, y, sx, sy, w = struct.unpack_from('<5f', out, at)
            struct.pack_into('<5f', out, at, x, y, 0.0, 0.0, w)
            hidden += 1
    return bytes(out), moved, hidden


# ---------------------------------------------------------------- the layouts' shape

# A layout after its header (+0x10: resources, nodes, groups, 1) and its resources (0x4C each): groups, each a u32
# count and that many 0x90-byte node records (+0x00 resource id, +0x04 x, y, scale x / y, width, height, +0x4C the
# resource's name, +0x5C the node's), the first group the root ("Zero"); then 4 bytes. 2011's (and 2010's) have 7:
# the root, four text groups (each the frame first: the names line, the champion's, two others), "beta" alone, then
# the background. SvR 2009's put beta with the background (6 groups) and SvR 2008's have no beta and three text lines
# more per group (main / main2 / sub2) - 2011's VS screen then shows no superstars and names (2009) or faults
# walking its elements (2008). reshape() gives them 2011's shape.
REC = 0x90


def node_name(r):
    return r[0x5C:0x6C].split(b'\0')[0].decode(errors='replace')


def parse_layout(y):
    """-> (header + resources, groups (lists of node records), the 4 closing bytes). Two forms: each group a count
    and its records (2009 - 2011), or (SvR 2008) every record in a row, then a table of (count, first record)
    per group."""
    nres, nnodes, ngroups, one = struct.unpack_from('<4I', y, 16)
    start = 0x20 + nres * 0x4C
    table = start + nnodes * REC
    if table + ngroups * 8 + 4 == len(y):  # (the table form: its size says so)
        recs = [y[start + i * REC:start + (i + 1) * REC] for i in range(nnodes)]
        groups = []
        for g in range(ngroups):
            n, first = struct.unpack_from("<II", y, table + g * 8)
            groups.append(recs[first:first + n])
        return y[:start], groups, y[-4:]
    p, groups = start, []
    for _ in range(ngroups):
        n = struct.unpack_from('<I', y, p)[0] if p + 4 <= len(y) else -1
        if n < 0 or n > nnodes or p + 4 + n * REC > len(y):
            break
        groups.append([y[p + 4 + i * REC:p + 4 + (i + 1) * REC] for i in range(n)])
        p += 4 + n * REC
    if len(groups) != ngroups or sum(map(len, groups)) != nnodes or p != len(y) - 4:
        raise ValueError('a layout of an unknown shape')
    return y[:start], groups, y[p:]


def write_layout(head, groups, tail):
    out = bytearray(head)
    struct.pack_into('<II', out, 0x14, sum(len(g) for g in groups), len(groups))
    for g in groups:
        out += struct.pack('<I', len(g)) + b''.join(g)
    return bytes(out + tail)


def reshape(y, y11):
    """The old layout in 2011's shape (y11: 2011's layout of that theme): its four text groups without the lines
    2011 never fills, with 2011's dummy_tonaight where 2011's group has one; then beta alone (the old one, else
    2011's), then the rest (the background ...). Already 2011's shape (7 groups): as it is. -> (layout, notes)"""
    head, groups, tail = parse_layout(y)
    _, g11, _ = parse_layout(y11)
    if len(groups) == len(g11) == 7 or len(groups) < 5 or len(g11) != 7:
        return y, []
    used = {node_name(r) for g in g11 for r in g}
    notes = []
    out = [groups[0]]
    for i in range(1, 5):
        keep = [r for r in groups[i] if not node_name(r).startswith('font000_') or node_name(r) in used]
        dropped = len(groups[i]) - len(keep)
        for r in g11[i]:
            if node_name(r) not in {node_name(k) for k in keep} and not node_name(r).startswith('font000_') \
                    and node_name(r) != node_name(g11[i][0]):
                keep.append(r)  # (2011's dummy_tonaight: the "TONIGHT" line's place)
        out.append(keep)
        if dropped:
            notes.append('group %d: %d text lines 2011 never fills left out' % (i, dropped))
    rest = [r for g in groups[5:] for r in g]
    beta = [r for r in rest if node_name(r) == 'beta'][:1] or [r for r in g11[5] if node_name(r) == 'beta'][:1]
    out.append(beta)
    out.append([r for r in rest if r not in beta])
    notes.append('reshaped %d -> 7 groups' % len(groups))
    return write_layout(head, out, tail), notes


def layout_of(blob):
    """An M<nn>L entry -> (its PACH children, the YXML)"""
    kids = f.pach_read(unwrap(blob))
    return kids, unwrap(kids[0][1])


def with_layout(blob, y):
    kids = f.pach_read(unwrap(blob))
    first = kids[0][1]
    packed = f.bpe_encode(y, compress=True) if first[:4] == b'BPE ' else y
    kids = [(kids[0][0], packed)] + kids[1:]
    out = f.pach_write(kids)
    return f.bpe_encode(out, compress=True) if blob[:4] == b'BPE ' else out


# ---------------------------------------------------------------- the commands

def read_old(src):
    try:
        d = discfs.open(src)
    except Exception as e:  # noqa: BLE001
        raise BadInput('cannot read %s: %s' % (src, e))
    with d:
        for p in ('pac/menu/menu.pac', 'pac/menu/MatchHD.pac'):
            if not d.exists(p):
                raise BadInput('the game files have no %s (SvR 2007 has no VS themes of this kind)' % p)
        return d.read('pac/menu/menu.pac'), d.read('pac/menu/MatchHD.pac')


def list_themes(game, src):
    """One item: the game's VS style (the themes it brings, by show)."""
    menu_raw, match_raw = read_old(src)
    _, menu = entries(menu_raw)
    _, match = entries(match_raw)
    sh = shows(game)
    have = [sh[n][0] for n11, n in sorted(theme_map(game).items())
            if all(('M%02d' % n + x) in d for x, d in (('L', menu), ('I', match), ('F', match)))]
    return [{'key': 'style', 'name': '%s VS screens' % GAMES[game], 'shows': ', '.join(have), 'themes': len(have),
             'in_2011': None, 'status': 'ok' if have else 'special',
             'reason': '' if have else 'no VS theme of this disc fits 2011'}]


def matchhd11(pac11):
    """--pac11: the 2011 game folder, its pac folder or MatchHD.pac itself"""
    for c in (pac11, os.path.join(pac11, 'menu', 'MatchHD.pac'), os.path.join(pac11, 'pac', 'menu', 'MatchHD.pac')):
        if os.path.isfile(c):
            return c
    raise BadInput('no 2011 MatchHD.pac at %s (--pac11: the 2011 game folder)' % pac11)


def convert(game, src, pac11, out, name=None, keep_text=False):
    menu_raw, match_raw = read_old(src)
    h_old, menu = entries(menu_raw)
    _, match = entries(match_raw)
    h11, match11 = entries(open(matchhd11(pac11), 'rb').read())
    sh = shows(game)
    fallback = tuple(layout_of(match11[k])[1] for k in ('M00L', 'M01L') if k in match11)
    ents, themes, notes = [], [], []
    for n11, n in sorted(theme_map(game).items()):
        k, k11 = 'M%02d' % n, 'M%02d' % n11
        if not all(x in d for x, d in ((k + 'L', menu), (k + 'I', match), (k + 'F', match))):
            notes.append('%s: no theme %02d on the disc - 2011\'s stays' % (sh[n][0], n))
            continue
        if k11 + 'L' not in match11:
            notes.append('2011 has no theme %02d - %s left out' % (n11, sh[n][0]))
            continue
        _, y = layout_of(menu[k + 'L'])
        _, y11 = layout_of(match11[k11 + 'L'])
        if not y.startswith(b'YXMLBIN VER0002') or not y11.startswith(b'YXMLBIN VER0002'):
            notes.append('%s: an unknown layout format - left out' % sh[n][0])
            continue
        if not keep_text:
            y, shaped = reshape(y, y11)
            notes += ['%s: %s' % (sh[n][0], x) for x in shaped if x.startswith('reshaped')][:1]
        y2, moved, hidden = (y, 0, 0) if keep_text else move_text(y, y11, fallback)
        ents += [((k11 + 'L').encode(), with_layout(menu[k + 'L'], y2)),
                 ((k11 + 'I').encode(), match[k + 'I']), ((k11 + 'F').encode(), match[k + 'F'])]
        themes.append({'theme': n11, 'from': n, 'show': sh[n][0], 'text_moved': moved, 'text_hidden': hidden})
    if not themes:
        raise BadInput('no VS theme of this disc fits 2011')
    folder = out
    os.makedirs(folder, exist_ok=True)
    data = f.epac_write(h11, [(b'MENU', ents)])
    tmp = os.path.join(folder, 'vs.pac.tmp')
    with open(tmp, 'wb') as fh:
        fh.write(data)
    os.replace(tmp, os.path.join(folder, 'vs.pac'))
    with open(os.path.join(folder, 'manifest.txt'), 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('type=VsStyle\nname=%s\ngame=%s\nthemes=%s\n'
                 % (name or GAMES[game], game, ','.join('%02d' % t['theme'] for t in themes)))
    shown = ', '.join('%s (%02d)' % (t['show'], t['theme']) for t in themes)
    notes = ["%d VS themes: %s; the others stay 2011's" % (len(themes), shown)] + notes
    if any(t['text_hidden'] for t in themes):
        notes.append("text lines 2011 never fills were hidden (%s's extra lines)" % GAMES[game])
    return {'ok': True, 'out': os.path.abspath(folder), 'opens': 'vs_style', 'install_as': game,
            'warnings': [], 'dropped': [], 'notes': notes, 'themes': themes, 'size': len(data)}


# ---------------------------------------------------------------- one theme for one arena mod

# An arena mod's own VS screen: <mod>/vs/theme.pac, an EPAC with group MENU and the entries VSTL / VSTI / VSTF (the
# layout, pictures and font of one theme). The game shows it on that arena's VS screen (vs_styles.cpp: the screen's
# /MENU/M<nn>x becomes /VA<k>/VSTx). Any theme of any supported disc, 2011's own included (--game svr2011, --src the
# 2011 game folder); its pictures replaced one by one (replace), its font taken from another theme (font).
THEME_KEYS = ('VSTL', 'VSTI', 'VSTF')
SHOWS11 = {0: 'SmackDown', 1: 'RAW', 2: 'ECW', 3: 'Tribute to the Troops', 4: 'SummerSlam', 5: 'Breaking Point',
           6: 'Hell in a Cell', 7: 'Survivor Series', 8: 'TLC', 9: 'Royal Rumble', 10: 'Elimination Chamber',
           11: 'WrestleMania XXVI', 12: 'Backlash', 13: 'Judgment Day', 14: 'Night of Champions', 15: 'The Bash',
           16: 'Extreme Rules', 18: 'Bragging Rights'}  # (2011's own themes by arena number)


def theme_sources(game, src, pac11):
    """-> ({theme number: (L blob, I blob, F blob)}, {number: show}) of a disc, or of 2011 itself"""
    if game == 'svr2011':
        _, m = entries(open(matchhd11(src), 'rb').read())
        out = {}
        for k in m:
            if re.match(r'M\d\dL$', k) and k[:3] + 'I' in m and k[:3] + 'F' in m:
                out[int(k[1:3])] = (m[k], m[k[:3] + 'I'], m[k[:3] + 'F'])
        names = {n: SHOWS11.get(n, 'theme %02d' % n) for n in out}
        for n in range(60, 67):
            if n in names:
                names[n] = 'match type theme %d' % n
        return out, names
    menu_raw, match_raw = read_old(src)
    _, menu = entries(menu_raw)
    _, match = entries(match_raw)
    sh = shows(game)
    out = {}
    for k in menu:
        if re.match(r'M\d\dL$', k) and k[:3] + 'I' in match and k[:3] + 'F' in match:
            out[int(k[1:3])] = (menu[k], match[k[:3] + 'I'], match[k[:3] + 'F'])
    return out, {n: sh[n][0] if n in sh else 'theme %02d' % n for n in out}


def biggest_picture(i_blob):
    """the theme's biggest picture (name, dds) - its background, for a preview"""
    best = None
    for name, dds in pictures(i_blob):
        w, h = struct.unpack_from('<II', dds, 16)[::-1]
        if best is None or w * h > best[0]:
            best = (w * h, name, dds)
    return best[1:] if best else (None, None)


def pictures(i_blob):
    """[(name.dds, dds bytes)] of a theme's I entry (PACH child 0x1B59, a texture bundle)"""
    from arena_tool import tex_bundle
    for eid, b in f.pach_read(unwrap(i_blob)):
        if eid == 0x1B59:
            return tex_bundle(unwrap(b))
    return []


def png_of(dds, path, width=None):
    from PIL import Image
    import io
    im = Image.open(io.BytesIO(dds)).convert('RGBA')
    if width:
        im = im.resize((width, max(1, im.height * width // im.width)))
    im.save(path)


def cmd_themes(game, src, pac11, out):
    """every theme of a disc (or 2011's own) with a preview PNG of its background"""
    os.makedirs(out, exist_ok=True)
    themes, names = theme_sources(game, src, pac11)
    items = []
    for n in sorted(themes):
        name, dds = biggest_picture(themes[n][1])
        prev = os.path.join(out, '%s_%02d.png' % (game, n))
        if dds and not os.path.exists(prev):
            try:
                png_of(dds, prev, 320)
            except Exception:  # noqa: BLE001
                prev = ''
        items.append({'key': '%02d' % n, 'name': names[n], 'preview': prev if dds else '',
                      'status': 'research' if game in RESEARCH else 'ok', 'reason': RESEARCH.get(game, '')})
    return items


def write_theme_pac(path, l_blob, i_blob, f_blob, head):
    data = f.epac_write(head, [(b'MENU', [(b'VSTL', l_blob), (b'VSTI', i_blob), (b'VSTF', f_blob)])])
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'wb') as fh:
        fh.write(data)
    os.replace(tmp, path)


def read_theme_pac(path):
    h, e = entries(open(path, 'rb').read())
    if not all(k in e for k in THEME_KEYS):
        raise BadInput('%s is not a VS theme pac (VSTL / VSTI / VSTF)' % path)
    return h, e


def cmd_theme(game, src, pac11, number, out):
    """one theme into <out> (a theme pac): an older game's rebuilt in 2011's shape, its text lines at 2011's places"""
    themes, names = theme_sources(game, src, pac11)
    if number not in themes:
        raise BadInput('%s has no VS theme %02d' % (GAMES.get(game, game), number))
    l_blob, i_blob, f_blob = themes[number]
    h11, match11 = entries(open(matchhd11(pac11), 'rb').read())
    notes = []
    if game != 'svr2011':
        n11 = {v: k for k, v in theme_map(game).items()}.get(number, 1)
        ref = 'M%02dL' % n11 if 'M%02dL' % n11 in match11 else 'M01L'
        _, y = layout_of(l_blob)
        _, y11 = layout_of(match11[ref])
        fallback = tuple(layout_of(match11[k])[1] for k in ('M00L', 'M01L') if k in match11)
        y, shaped = reshape(y, y11)
        y, moved, hidden = move_text(y, y11, fallback)
        l_blob = with_layout(l_blob, y)
        notes += shaped + ['%d text lines at 2011\'s places (as its theme %s)' % (moved, ref[1:3])]
    write_theme_pac(out, l_blob, i_blob, f_blob, h11)
    return {'ok': True, 'out': os.path.abspath(out), 'opens': 'vs_theme', 'theme': '%s %02d' % (game, number),
            'name': names[number], 'warnings': [], 'dropped': [], 'notes': notes}


def cmd_parts(theme_pac, out):
    """a theme pac's pictures: name, size, format and a PNG thumbnail each"""
    _, e = read_theme_pac(theme_pac)
    os.makedirs(out, exist_ok=True)
    items = []
    for name, dds in pictures(e['VSTI']):
        h, w = struct.unpack_from('<II', dds, 12)
        thumb = os.path.join(out, name.rsplit('.', 1)[0] + '.png')
        try:
            png_of(dds, thumb, min(256, w))
        except Exception:  # noqa: BLE001
            thumb = ''
        items.append({'key': name, 'width': w, 'height': h, 'format': dds[84:88].decode('latin1').strip('\0'),
                      'thumb': thumb})
    return items


def cmd_replace(theme_pac, part, png):
    """one picture of a theme pac replaced by a PNG (fitted to the picture's size, its own DXT format, mips)"""
    from PIL import Image
    import svr08_char
    from arena_tool import tex_bundle, tex_bundle_write
    h, e = read_theme_pac(theme_pac)
    kids = f.pach_read(unwrap(e['VSTI']))
    out_kids, done = [], None
    for eid, b in kids:
        if eid == 0x1B59:
            texs = tex_bundle(unwrap(b))
            for k, (name, dds) in enumerate(texs):
                if name.lower() == part.lower() or name.rsplit('.', 1)[0].lower() == part.lower():
                    hh, ww = struct.unpack_from('<II', dds, 12)
                    fourcc = dds[84:88].decode('latin1').strip('\0') or 'DXT5'
                    im = Image.open(png).convert('RGBA').resize((ww, hh), Image.LANCZOS)
                    texs[k] = (name, svr08_char.dds_encode(im, fourcc if fourcc in ('DXT1', 'DXT5') else 'DXT5'))
                    done = (name, ww, hh)
            raw = tex_bundle_write(texs)
            b = f.bpe_encode(raw, compress=True)
        out_kids.append((eid, b))
    if not done:
        raise BadInput('the theme has no picture %s' % part)
    e['VSTI'] = f.pach_write(out_kids)
    write_theme_pac(theme_pac, e['VSTL'], e['VSTI'], e['VSTF'], h)
    return {'ok': True, 'out': os.path.abspath(theme_pac), 'replaced': done[0], 'size': '%dx%d' % done[1:]}


def cmd_font(theme_pac, game, src, pac11, number):
    """a theme pac with the font of another theme (any disc's, or 2011's)"""
    h, e = read_theme_pac(theme_pac)
    themes, names = theme_sources(game, src, pac11)
    if number not in themes:
        raise BadInput('%s has no VS theme %02d' % (GAMES.get(game, game), number))
    write_theme_pac(theme_pac, e['VSTL'], e['VSTI'], themes[number][2], h)
    return {'ok': True, 'out': os.path.abspath(theme_pac), 'font': '%s %02d (%s)' % (game, number, names[number])}


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog='vs_port.py')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('games')
    p = sub.add_parser('list')
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--pac11')  # (the Mod Maker passes it to every list; not needed here)
    p = sub.add_parser('convert')
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--pac11', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--item', default='style')
    p.add_argument('--name')
    p.add_argument('--renders', action='store_true')
    p.add_argument('--keep-text', action='store_true', help=argparse.SUPPRESS)  # (test aid: the old layout as it is)
    p = sub.add_parser('themes')  # (every theme of a disc, or --game svr2011 --src <2011 game>: 2011's own)
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--pac11')
    p.add_argument('--out', required=True)
    p = sub.add_parser('theme')
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--pac11', required=True)
    p.add_argument('--theme', type=int, required=True)
    p.add_argument('--out', required=True)
    p = sub.add_parser('parts')
    p.add_argument('--pac', required=True)
    p.add_argument('--out', required=True)
    p = sub.add_parser('replace')
    p.add_argument('--pac', required=True)
    p.add_argument('--part', required=True)
    p.add_argument('--png', required=True)
    p = sub.add_parser('font')
    p.add_argument('--pac', required=True)
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--pac11')
    p.add_argument('--theme', type=int, required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd in ('themes', 'theme', 'font') and a.game not in GAMES and a.game != 'svr2011':
            raise BadInput('unknown game %r (%s, svr2011)' % (a.game, ', '.join(GAMES)))
        if a.cmd == 'themes':
            print(json.dumps(cmd_themes(a.game, a.src, a.pac11, a.out)))
            return 0
        if a.cmd == 'theme':
            print(json.dumps(cmd_theme(a.game, a.src, a.pac11, a.theme, a.out)))
            return 0
        if a.cmd == 'parts':
            print(json.dumps(cmd_parts(a.pac, a.out)))
            return 0
        if a.cmd == 'replace':
            print(json.dumps(cmd_replace(a.pac, a.part, a.png)))
            return 0
        if a.cmd == 'font':
            print(json.dumps(cmd_font(a.pac, a.game, a.src, a.pac11, a.theme)))
            return 0
        if a.cmd == 'games':
            print(json.dumps([{'id': k, 'name': v, 'platform': 'x360', 'disc_hint': DISC[k],
                               'status': 'research' if k in RESEARCH else 'ready'} for k, v in GAMES.items()]))
            return 0
        if a.game not in GAMES:
            raise BadInput('unknown game %r (%s)' % (a.game, ', '.join(GAMES)))
        if a.game in RESEARCH and not getattr(a, 'keep_text', False):  # (--keep-text: the crash's test aid)
            raise BadInput(RESEARCH[a.game])
        if a.cmd == 'list':
            print(json.dumps(list_themes(a.game, a.src)))
            return 0
        rep = convert(a.game, a.src, a.pac11, a.out, a.name, a.keep_text)
        if a.renders:
            import vs_renders
            rep['renders'] = vs_renders.convert(a.game, a.src, a.pac11, os.path.join(a.out, 'renders'))
            rep['notes'] += rep['renders'].get('notes', [])
        with open(os.path.join(a.out, 'report.json'), 'w', encoding='utf-8') as fh:
            json.dump(rep, fh, indent=1)
        print(json.dumps(rep))
        return 0
    except BadInput as e:
        print(json.dumps({'error': str(e)}))
        return 2
    except Exception as e:  # noqa: BLE001
        print(json.dumps({'error': 'conversion failed: %s' % e}))
        return 3


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
