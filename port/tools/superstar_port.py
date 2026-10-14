"""Superstars from other WWE games as SvR 2011 superstar mods - the converter
the Mod Maker calls (docs/MOD_MAKER_3_PLAN.md section 4, docs/OTHER_GAMES.md).

    py superstar_port.py games
    py superstar_port.py list    --game <id> --src <ISO or extract folder> [--pac11 <2011 pac>]
    py superstar_port.py convert --game <id> --src <...> --item <key> --out <dir> [options]

stdout is only JSON; logs go to stderr, with progress lines `progress <0-100> <text>`.
Exit 0 ok, 2 bad input, 3 failed.

convert writes <dir>/<mod id>.svrmod, the same mod unpacked in <dir>/<mod id>/, its
work files in <dir>/.work_<key>/, and <dir>/report.json (also on stdout):
    {ok, out, folder, opens: "superstar", warnings[], dropped[], notes[]}

Options (convert):
    --base N        the 2011 superstar the mod starts from (default: the same superstar
                    when 2011 has him, else the closest by gender, weight class, size and moves)
    --entrance N    2011 entrance number (default: the one with his name, else a generic one)
    --call N        commentary nickname 0-83
    --height S      height=S (0.80-1.25)
    --model src|11  the model from the source game (default) or 2011's own when it has one
    --no-moves      keep the base's moves
    --no-port       his moves 2011 has, but port none (WWE '12 / '13 / 2K14: faster)
    --max-port N    port at most N moves (default 40)
    --no-taunts     keep the base's taunts (his own are ported by default: YMBs moves, moves_port)
    --same-moves    PS2 games: only 2011's same moves (no 'similar' ones, tools/ps2_moves.py)

Games:
    svr2010   SvR 2010 - everything (svr10_superstar.py: model, renders, theme, titantron,
              ratings, abilities, the moveset and the moves 2011 lacks as a move pack)
    svr2009   SvR 2009 - model (svr08_char.py), renders, theme, titantron, ratings, entrance,
              announcer; the moveset stays the base's for now (the 2009 slot map is next)
    svr2008   SvR 2008 - the same as 2009 (ids: 2011 id = 2008 id + 100)
    svr2007   SvR 2007 - the same, its per-part textures baked (svr07_char.py), XACT themes (xact.py);
              ratings stay the base's
    wwe12     WWE '12 - model (w13_char.py, within the vertex budget), attires, renders, theme
              (wwise.py + vgmstream), entrance + titantron (entrance_port.py; Bink 2 opt in), announcer;
              ratings and moveset stay the base's
    wwe13     WWE '13 - the same, from its xcompressed pacs (--svrmod)
    wwe2k14   WWE 2K14 - the same, names from string.pac, the first 4 of its attires, themes from
              Game_Sound_E_S.pck events + Game_Sound_M.pck streams
    others    listed with status "research" (docs/OTHER_GAMES.md)

--src may be a disc image (read through discfs.py); the files a conversion needs are
copied once to <dir>/.src_<game>/ (pac/ch/chEtc.pac, DLC_HD.pac, the superstar's ch pac,
evt/Nyujyo*.pac, the music bank, the titantron; for 2010 moves also m.pac / misc.pac).
"""
import collections
import contextlib
import io
import json
import math
import os
import re
import shutil
import struct
import sys
import traceback
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ch_tool  # noqa: E402
import discfs  # noqa: E402
import svrfmt  # noqa: E402

PAC11 = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'Game Files', 'pac')

GAMES = [
    dict(id='svr2010', name='WWE SmackDown vs. Raw 2010', platform='x360', disc_hint='54510844', status='ready'),
    dict(id='svr2009', name='WWE SmackDown vs. Raw 2009', platform='x360', disc_hint='54510826', status='ready'),
    dict(id='svr2008', name='WWE SmackDown vs. Raw 2008', platform='x360', disc_hint='5451080B', status='ready'),
    dict(id='svr2007', name='WWE SmackDown vs. Raw 2007', platform='x360', disc_hint='545107E0', status='ready'),
    dict(id='ps2_sym', name='WWE SmackDown! Shut Your Mouth', platform='ps2', disc_hints=['SLES_512.83', 'SLUS_205.12'], status='ready'),
    dict(id='ps2_hctp', name='WWE SmackDown! Here Comes the Pain', platform='ps2', disc_hints=['SLES_520.36', 'SLUS_207.87'], status='ready'),
    dict(id='ps2_svr04', name='WWE SmackDown! vs. Raw (PS2)', platform='ps2', disc_hints=['SLES_527.81', 'SLUS_209.36'], status='ready'),
    dict(id='ps2_svr06', name='WWE SmackDown! vs. Raw 2006 (PS2)', platform='ps2', disc_hints=['SLES_536.76', 'SLUS_211.34'], status='ready'),
    dict(id='ps2_svr07', name='WWE SmackDown vs. Raw 2007 (PS2)', platform='ps2', disc_hints=['SLES_544.89', 'SLUS_214.27'], status='ready'),
    dict(id='ps2_svr08', name='WWE SmackDown vs. Raw 2008 (PS2)', platform='ps2', disc_hints=['SLES_548.79'], status='ready'),
    dict(id='ps2_svr09', name='WWE SmackDown vs. Raw 2009 (PS2)', platform='ps2', disc_hints=['SLES_552.51'], status='ready'),
    dict(id='ps2_svr10', name='WWE SmackDown vs. Raw 2010 (PS2)', platform='ps2', disc_hints=['SLES_555.45'], status='ready'),
    dict(id='wwe12', name="WWE '12", platform='x360', disc_hint='54510890', status='ready'),
    dict(id='wwe13', name="WWE '13", platform='x360', disc_hint='545108B4', status='ready'),
    dict(id='wwe2k14', name='WWE 2K14', platform='x360', disc_hint='545408B2', status='ready'),
    dict(id='wwe2k15', name='WWE 2K15', platform='x360', disc_hint='545408B6', status='ready'),
    dict(id='wwe2k16', name='WWE 2K16', platform='x360', disc_hint='315A07D2', status='ready'),
    dict(id='wwe2k17', name='WWE 2K17', platform='x360', disc_hint='545408BA', status='ready'),
]
READY = {g['id'] for g in GAMES if g['status'] == 'ready'}


class BadInput(Exception):
    pass


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def progress(pct, text):
    print('progress %d %s' % (pct, text), file=sys.stderr, flush=True)


# --------------------------------------------------------------------------- source

class Src:
    """The source game: a folder is used in place; from a disc image the files a step asks
    for are copied once to a cache folder, so the existing readers (which take paths) work."""

    def __init__(self, path, cache):
        if not os.path.exists(path):
            raise BadInput('--src not found: %s' % path)
        self.d = discfs.open(path)
        self.path = path
        self.folder = path if self.d.kind == 'folder' else None
        self.cache = cache
        if not self.d.exists('pac'):
            raise BadInput('--src has no pac folder: %s' % path)

    @property
    def root(self):
        return self.folder or self.cache

    def has(self, rel):
        return self.d.exists(rel)

    def get(self, rel):
        """local path of rel (copied from the image when needed), or None"""
        if not self.d.exists(rel):
            return None
        if self.folder:
            return os.path.join(self.folder, *rel.split('/'))
        dst = os.path.join(self.cache, *rel.split('/'))
        if not os.path.exists(dst) or os.path.getsize(dst) != self.d.size(rel):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            tmp = dst + '.part'
            with open(tmp, 'wb') as f:
                size, off = self.d.size(rel), 0
                while off < size:
                    n = min(64 << 20, size - off)
                    f.write(self.d.read(rel, off, n))
                    off += n
            os.replace(tmp, dst)
        return dst

    def get_dir(self, rel, pred=lambda n: True):
        for n in self.d.listdir(rel):
            if pred(n):
                self.get(rel + '/' + n)
        return os.path.join(self.root, *rel.split('/'))

    def info(self):
        try:
            return self.d.info()
        except Exception:   # noqa: BLE001
            return {}


# --------------------------------------------------------------------------- shared 2011 side

def svr10():
    import svr10_superstar as S   # heavy imports (svr10_moves) only when needed
    return S


def game11(pac11):
    S = svr10()
    if not os.path.exists(os.path.join(pac11, 'ch', 'chEtc.pac')):
        raise BadInput('--pac11 is not an SvR 2011 pac folder: %s' % pac11)
    return S.Game(pac11, 11), S.Sound11(pac11)


def playable11(g11):
    return set(svr10().candidates_11(g11))


# --------------------------------------------------------------------------- SvR 2009

class Game09:
    """SvR 2009 = 2008 models in 2010's file layout (docs/OTHER_GAMES.md): chEtc.pac CHAR/DAT
    (same offsets as 2010 after the 4-byte header: name +34, short +170, gender +208, weight
    class +209, scale +28, ratings +0..+7) and CHAR/PRO (496 B, entrance block +0xE4); renders
    in DLC_HD.pac MENU/SSFA|SSFB|SSFC = PACH{0: PACH{id: DDS}} (SSFA/B 256 x 512, SSFC 256);
    EMD entries named by 2 hex digits of the id + attire + kind; evt/Nyujyo*.pac are EPAC."""

    year = 9
    label = 'SvR 2009'
    suffix = '_09'

    def id11(self, i):
        return i

    def __init__(self, src):
        S = svr10()
        self.src = src
        pac = src.get_dir('pac/ch', lambda n: n.lower() == 'chetc.pac').rsplit(os.sep, 1)[0]
        self.g = S.Game.__new__(S.Game)   # the 2010 reader's record helpers, 2009 tables
        self.g.pac, self.g.year = pac, 9
        h, g, t = svrfmt.epac_read(open(os.path.join(pac, 'ch', 'chEtc.pac'), 'rb').read())
        ch = {(ty, n): b for ty, es in g for n, b in es}

        def pach(b):
            return {i: ch_tool.unpack(d) for i, d in svrfmt.pach_read(ch_tool.unpack(b))}
        self.g.dat = {i: r[4:] for i, r in pach(ch[(b'CHAR', b'DAT ')]).items()}
        self.g.pro = pach(ch[(b'CHAR', b'PRO ')])
        self.g.ssf, self.g._evp, self.g._waze = {}, None, None
        self._ssf = None
        self._evp = None

    # records
    def name(self, i): return self.g.name(i)
    def short(self, i): return self.g.short(i)
    def named(self, i): return self.g.named(i)

    def entrance_block(self, i):
        return struct.unpack_from('<4H', self.g.pro[i], 0xE4)

    def ssf(self):
        if self._ssf is None:
            self._ssf = {}
            p = self.src.get('pac/DLC_HD.pac')
            h, g, t = svrfmt.epac_read(open(p, 'rb').read())
            for ty, es in g:
                for n, b in es:
                    if ty == b'MENU' and n in (b'SSFA', b'SSFB', b'SSFC'):
                        outer = svrfmt.pach_read(ch_tool.unpack(b))
                        for i, d in svrfmt.pach_read(ch_tool.unpack(outer[0][1])):
                            self._ssf[(n.decode(), i)] = d
        return self._ssf

    def chpac(self, i):
        return self.src.get('pac/ch/ch%d.pac' % i)

    def has_chpac(self, i):
        return self.src.has('pac/ch/ch%d.pac' % i)

    def evp(self):
        """{entrance number: name} from evt/Nyujyo*.pac (EPAC; EVP name at +0x22)"""
        if self._evp is None:
            self._evp = {}
            for n in self.src.d.listdir('pac/evt'):
                if not n.lower().startswith('nyujyo'):
                    continue
                h, g, t = svrfmt.epac_read(open(self.src.get('pac/evt/' + n), 'rb').read())
                for ty, es in g:
                    if ty[:3] != b'EVP':
                        continue
                    for nm, b in es:
                        if nm.strip(b'\0 ').isdigit():
                            d = ch_tool.unpack(b)
                            self._evp[int(nm)] = d[0x22:0x42].split(b'\0')[0].decode('latin1').strip()
        return self._evp

    def music_records(self):
        """{record id: (path, offset, size)} over sound/music*.yaf"""
        S = svr10()
        out = {}
        for n in sorted(self.src.d.listdir('sound')):
            if re.match(r'music\d*\.yaf$', n.lower()):
                p = self.src.get('sound/' + n)
                for rid, (off, size) in S.yaf_table(p).items():
                    out.setdefault(rid, (p, off, size))
        return out

    def titantron(self, n):
        return self.src.get('movies/titantron/%03d.bik' % n)

    def roster(self):
        return [c for c in sorted(self.g.dat) if 100 <= c < 400 and self.named(c)
                and not self.name(c).upper().startswith(('MOTION', 'MOVES', 'EDIT', 'DLC', 'DUMMY', 'MATERIAL', 'LEGEND MOTEST'))
                and self.has_chpac(c)]


class Game08(Game09):
    """SvR 2008 (docs/OTHER_GAMES.md): misc.pac CHAR/DAT (232 B: name +0x20, ratings +4..+11,
    gender +206, weight class +207, scale +26) and CHAR/PRO (450 B, no entrance block: the theme
    is music.yaf record id*100, the titantron movies/titantron/%02d.bik, the entrance 4000 + id);
    renders menu/menuHD.pac MENU/SSFA (512) / SSFB (512 x 256) / SSFC (256), nested PACH by id;
    ch/chNN.pac with EMD names id attire kind in decimal. 2011 id = 2008 id + 100."""

    year = 8
    label = 'SvR 2008'
    suffix = '_08'

    def id11(self, i):
        return i + 100

    def __init__(self, src):
        S = svr10()
        self.src = src
        misc = src.get('pac/misc.pac')
        h, g, t = svrfmt.epac_read(open(misc, 'rb').read())
        ch = {(ty, n): b for ty, es in g for n, b in es}

        def pach(b):
            return {i: ch_tool.unpack(d) for i, d in svrfmt.pach_read(ch_tool.unpack(b))}
        self.g = S.Game.__new__(S.Game)
        self.g.pac, self.g.year = os.path.dirname(misc), 8
        self.g.dat = {}
        self.raw_dat = pach(ch[(b'CHAR', b'DAT ')])   # (as stored: abilities_port reads the fighting styles, +227 / +228)
        for i, r in self.raw_dat.items():
            # laid out as a 2010 record (after its 4-byte header) for the shared helpers
            x = bytearray(264)
            x[0:8] = r[4:12]
            nm = r[0x20:0x60].split(b'\0')[0][:36]
            x[34:34 + len(nm)] = nm
            x[170:170 + len(nm)] = nm
            x[28:30] = r[26:28]
            x[208], x[209] = r[206], r[207]
            self.g.dat[i] = bytes(x)
        self.g.pro = pach(ch[(b'CHAR', b'PRO ')])
        self.g.ssf, self.g._evp, self.g._waze = {}, None, None
        self._ssf = None
        self._evp = None

    def entrance_block(self, i):
        return (i, i, 4000 + i, 0)

    def ssf(self):
        if self._ssf is None:
            self._ssf = {}
            h, g, t = svrfmt.epac_read(open(self.src.get('pac/menu/menuHD.pac'), 'rb').read())
            for ty, es in g:
                for n, b in es:
                    if ty == b'MENU' and n in (b'SSFA', b'SSFB', b'SSFC'):
                        outer = svrfmt.pach_read(ch_tool.unpack(b))
                        for i, d in svrfmt.pach_read(ch_tool.unpack(outer[0][1])):
                            self._ssf[(n.decode(), i)] = d
        return self._ssf

    def chpac(self, i):
        return self.src.get('pac/ch/ch%02d.pac' % i)

    def has_chpac(self, i):
        return self.src.has('pac/ch/ch%02d.pac' % i)

    def titantron(self, n):
        return self.src.get('movies/titantron/%02d.bik' % n)

    def roster(self):
        return [c for c in sorted(self.g.dat) if c < 100 and self.named(c)
                and not self.name(c).upper().startswith(('MOTION', 'MOVES', 'EDIT', 'DLC', 'DUMMY'))
                and self.has_chpac(c)]


class Game07(Game08):
    """SvR 2007 (docs/OTHER_GAMES.md): 2008's id space (2011 id = id + 100); misc.pac CHAR/DAT
    176 B (name +0x1C, weight class +147 with 0 = diva, scale +22; its ratings use another scale:
    left to the base); renders menu/menuHD.pac MENU SF2B (512 body) / SF2A (256 bust); models
    with per-part textures (svr07_char.py bakes them); themes XACT (xact.py); titantron %02d.bik."""

    year = 7
    label = 'SvR 2007'
    suffix = '_07'

    def __init__(self, src):
        S = svr10()
        self.src = src
        misc = src.get('pac/misc.pac')
        h, g, t = svrfmt.epac_read(open(misc, 'rb').read())
        ch = {(ty, n): b for ty, es in g for n, b in es}

        def pach(b):
            return {i: ch_tool.unpack(d) for i, d in svrfmt.pach_read(ch_tool.unpack(b))}
        self.g = S.Game.__new__(S.Game)
        self.g.pac, self.g.year = os.path.dirname(misc), 7
        self.g.dat = {}
        self.raw_dat = pach(ch[(b'CHAR', b'DAT ')])   # (as stored: ratings_port reads its 8 ratings, a 7..20 scale)
        for i, r in self.raw_dat.items():
            x = bytearray(264)
            nm = r[0x1C:0x4B].split(b'\0')[0][:32]
            x[34:34 + len(nm)] = nm
            x[170:170 + len(nm)] = nm
            x[28:30] = r[22:24]
            x[208] = 1 if r[147] == 0 else 0
            x[209] = r[147]
            self.g.dat[i] = bytes(x)
        self.g.pro = pach(ch[(b'CHAR', b'PRO ')])
        self.g.ssf, self.g._evp, self.g._waze = {}, None, None
        self._ssf = None
        self._evp = None

    def ssf(self):
        if self._ssf is None:
            self._ssf = {}
            h, g, t = svrfmt.epac_read(open(self.src.get('pac/menu/menuHD.pac'), 'rb').read())
            for ty, es in g:
                for n, b in es:
                    kind = {b'SF2B': 'SSFB', b'SF2A': 'SSFC'}.get(n)
                    if ty != b'MENU' or not kind:
                        continue
                    outer = svrfmt.pach_read(ch_tool.unpack(b))
                    if len(outer) == 1 and ch_tool.unpack(outer[0][1])[:4] == b'PACH':
                        outer = svrfmt.pach_read(ch_tool.unpack(outer[0][1]))
                    for i, d in outer:
                        self._ssf[(kind, i)] = d
        return self._ssf

    def evp(self):
        """{entrance number: name} from evt/evt*.pac (EPAC, EVP groups; name at +0x22)"""
        if self._evp is None:
            self._evp = {}
            for n in self.src.d.listdir('pac/evt'):
                if not re.match(r'evt\d+\.pac$', n.lower()):
                    continue
                try:
                    h, g, t = svrfmt.epac_read(open(self.src.get('pac/evt/' + n), 'rb').read())
                except Exception:   # noqa: BLE001
                    continue
                for ty, es in g:
                    if ty[:3] != b'EVP':
                        continue
                    for nm, b in es:
                        if nm.strip(b'\0 ').isdigit():
                            d = ch_tool.unpack(b)
                            self._evp[int(nm)] = d[0x22:0x42].split(b'\0')[0].decode('latin1').strip()
        return self._evp


def pad_render(dds, size=512):
    """a 256 x 512 render on a transparent 512 x 512, DXT5 with mips"""
    from PIL import Image
    import svr08_char
    im = Image.open(io.BytesIO(dds)).convert('RGBA')
    if im.size == (size, size):
        return dds
    canvas = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    k = min(size / im.width, size / im.height, 1.0)
    if k != 1.0:
        im = im.resize((int(im.width * k), int(im.height * k)), Image.LANCZOS)
    canvas.paste(im, ((size - im.width) // 2, size - im.height))
    return svr08_char.dds_encode(canvas, 'DXT5')


def model_from_2008_family(src_pac, cid, pac11, out, portrait, hex_names):
    """svr08_char's conversion of the wrestler (kind 2) entries of a 2008-family ch pac
    (2008: EMD names id attire kind in decimal, 2009: 2 hex digits). -> [(entry, 2011 name)]"""
    import svr08_char
    tp = svr08_char.Template(os.path.join(pac11, 'ch', 'ch104.pac'))
    h, g, t = svrfmt.epac_read(open(src_pac, 'rb').read())
    emd = next(es for ty, es in g if ty == b'EMD ')
    ents = []
    for n, b in emd:
        s = n.decode('latin1').strip('\0 ')
        if len(s) != 4 or s[3] != '2':
            continue
        ents.append((s, int(s[2]), {i: svr08_char.unp(x) for i, x in svrfmt.pach_read(svr08_char.unp(b))}))
    if not ents:
        raise RuntimeError('no wrestler (kind 2) model in %s' % os.path.basename(src_pac))
    ents.sort(key=lambda e: e[1])
    first = ents[0][2]
    entries, kept = [], []
    import model_budget
    budget_notes = []
    for k, (s, att, kids) in enumerate(ents[:4]):
        o = svr08_char.attire(kids, tp, portrait, [0x20], first)
        # a wrestler's vertex buffers get 192 pages in a match (model_budget.py): over that the
        # match load crashes. Stock models without blood overlays exist (ch164): drop them first.
        pg, _ = model_budget.attire_pages(o)
        for drop in (0x7532, 0x7531, 0x7530):
            if pg <= model_budget.LIMIT:
                break
            if drop in o:
                del o[drop]
                pg, _ = model_budget.attire_pages(o)
                budget_notes.append('attire %s: blood overlay %#x left out (vertex memory)' % (s, drop))
        if pg > model_budget.LIMIT:
            budget_notes.append('attire %s: %d of %d pages of vertex memory - the match load may crash' % (s, pg, model_budget.LIMIT))
        name = b'%06d%02d' % (cid, k * 10 + 2)
        entries.append((name, svrfmt.pach_write([(i, svrfmt.bpe_encode(o[i], compress=True)) for i in sorted(o)])))
        kept.append((s, name.decode()))
    svr08_char.write_epk8(os.path.join(pac11, 'ch', 'ch104.pac'), entries, out)
    model_from_2008_family.notes = budget_notes
    return kept, [e[0] for e in ents[4:]]


def theme_from_record(rec, out_mp3, work):
    """a yaf record (XMA1/2) -> mp3 at the mod theme loudness"""
    import subprocess
    S = svr10()
    p, off, size = rec
    with open(p, 'rb') as f:
        f.seek(off)
        data = f.read(size)
    raw = os.path.join(work, 'theme.xma.wav')
    open(raw, 'wb').write(data)
    src = S.loudness(raw)
    gain = S.THEME_LUFS - src
    subprocess.run([S.ffmpeg(), '-hide_banner', '-loglevel', 'error', '-y', '-i', raw, '-af', 'volume=%.2fdB' % gain,
                    '-c:a', 'libmp3lame', '-b:a', '256k', out_mp3], check=True, stdout=subprocess.DEVNULL)
    return src, gain


def entrance_from_names(names, g11):
    S = svr10()
    e11 = {}
    for n, s in g11.evp().items():
        if n < 600:
            e11.setdefault(S.norm(s), n)
    for s in names:
        if s and S.norm(s) in e11:
            return e11[S.norm(s)], s
    return None, None


def write_mod(mod, man, out_svrmod, rep=None, pac11=None):
    """mod/manifest.txt from man, then mod/ zipped to out_svrmod. With pac11: the model (mod/ch.pac) kept within
    the match's vertex budget first (fit_budget - the game leaves an over-budget model out), what that takes
    out added to rep['dropped']"""
    if pac11 and os.path.exists(os.path.join(mod, 'ch.pac')):
        # bone rests lined up with the stock ones (tools/prop_bones.py, B3): an edited model's odd rests distort
        # it in motion; the pac is rewritten only when a bone is put back, and a failure never fails the port
        for f_ in sorted(x for x in os.listdir(mod) if x == 'ch.pac' or (x.startswith('attire') and x.endswith('.pac'))):
            try:
                import prop_bones
                pr = prop_bones.fix_pac(os.path.join(mod, f_), out=os.path.join(mod, f_))
                if pr['fixed'] and rep is not None:
                    rep['notes'].append('%s: %s' % (f_, prop_bones.report_line(pr)))
            except Exception as e:   # noqa: BLE001
                if rep is not None:
                    rep['warnings'].append('%s: bone rests not checked (prop_bones: %s)' % (f_, e))
        for d_ in fit_budget(os.path.join(mod, 'ch.pac'), os.path.join(pac11, 'ch', 'ch104.pac')):
            if rep is not None:
                rep['dropped'].append(d_)
    order = ['type', 'id', 'name', 'short', 'base', 'author', 'made_with', 'version', 'ratings', 'abilities', 'moves',
             'entrance', 'announcer', 'call', 'height', 'song', 'movie']
    txt = ''.join('%s=%s\n' % (k, man[k]) for k in order if k in man)
    txt += ''.join('%s=%s\n' % (k, v) for k, v in man.items() if k not in order)   # (attire<N>_name= ...)
    open(os.path.join(mod, 'manifest.txt'), 'w', newline='\n').write(txt)
    tmp = out_svrmod + '.tmp'
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(mod):
            for f in sorted(files):
                fp = os.path.join(root, f)
                z.write(fp, os.path.relpath(fp, mod).replace('\\', '/'))
    os.replace(tmp, out_svrmod)
    return txt


def choose_base_old(g, g11, cid):
    """the playable 2011 superstar of the same gender and weight class with the closest body scale
    (the 2008-family profiles use another move-id layout, so moves aren't compared)"""
    S = svr10()
    r = g.g.dat[cid]
    gender, wclass, scale = r[208], r[209], struct.unpack_from('<H', r, 28)[0]
    cands = [c for c in S.candidates_11(g11) if g11.dat[c][208] == gender]
    same = [c for c in cands if g11.dat[c][209] == wclass] or cands
    best = min(same, key=lambda c: abs(struct.unpack_from('<H', g11.dat[c], 28)[0] - scale))
    return best, 'gender %d, weight class %d, scale %d vs %d' % (gender, wclass, struct.unpack_from('<H', g11.dat[best], 28)[0], scale)


def abilities_step(game, g, cid, man, note):
    """the source's abilities as 2011's (tools/abilities_port.py) into man['abilities']; notes what was mapped
    or guessed (none: the base's)"""
    import abilities_port
    game = {'Game09': 'svr2009', 'Game08': 'svr2008', 'Game07': 'svr2007'}.get(game, game)
    ratings = [int(x) for x in man['ratings'].split(',')] if man.get('ratings') else None
    ids, ar = abilities_port.map_abilities(game, cid, g, ratings)
    if ids:
        man['abilities'] = ','.join(map(str, ids))
    for line in abilities_port.report_lines(getattr(g, 'label', game), ids, ar):
        note(line)


def build09(cid, outdir, o, rep):
    S = svr10()
    g9, g11, snd, pac11 = o['src_game'], o['g11'], o['snd'], o['pac11']
    if cid not in g9.g.dat or not g9.named(cid):
        raise BadInput('%s has no superstar %d' % (g9.label, cid))
    name = g9.name(cid)
    i11 = g9.id11(cid)
    mid = re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_') + g9.suffix
    work = os.path.join(outdir, '.work_%d' % cid)
    mod = os.path.join(outdir, mid)
    for d in (work, mod):
        if os.path.exists(d):
            shutil.rmtree(d)
        os.makedirs(d)
    note, warn, drop = rep['notes'].append, rep['warnings'].append, rep['dropped'].append
    progress(5, 'base superstar')
    same11 = i11 in playable11(g11) and S.norm(g11.name(i11)) == S.norm(name)
    if o.get('base'):
        base, why = o['base'], 'option'
    elif same11:
        base, why = i11, '2011 has him: his own moves, entrance and commentary'
    else:
        base, why = choose_base_old(g9, g11, cid)
    note('base %d %s (%s)' % (base, g11.name(base), why))
    man = collections.OrderedDict(type='superstar', id=mid, name=name[:31], short=(g9.short(cid) or name)[:31],
                                  base=base, author=g9.label + ' port', made_with='Port tools (%s)' % g9.label, version='1.0')
    # renders (and the portrait from the bust)
    progress(15, 'renders')
    ssf = g9.ssf()
    portrait = None
    if ('SSFC', cid) in ssf:
        portrait = ch_tool.unpack(ssf[('SSFC', cid)])
        open(os.path.join(mod, 'render_small.dds'), 'wb').write(portrait)
    body = ssf.get(('SSFB', cid)) or ssf.get(('SSFA', cid))
    if body:
        open(os.path.join(mod, 'render.dds'), 'wb').write(pad_render(ch_tool.unpack(body)))
        note('renders: %s SSFB/SSFC %d (padded to 512 x 512)' % (g9.label, cid))
    else:
        warn('no %s render: the base\'s are used' % g9.label)
    # model
    progress(25, 'model')
    if o.get('model') == '11' and same11 and g11.chpac(i11):
        shutil.copyfile(g11.chpac(i11), os.path.join(mod, 'ch.pac'))
        note('model: 2011\'s own ch%d.pac (--model 11)' % i11)
    elif g9.has_chpac(cid) and g9.year == 7:   # (per-part textures: baked into 2011's sheets)
        import svr07_char
        kept7, extra = svr07_char.convert(g9.chpac(cid), os.path.join(mod, 'ch.pac'), os.path.join(pac11, 'ch', 'ch104.pac'),
                                          cid, portrait)
        note('model: %s ch%02d.pac attires %s converted (svr07_char: %s)' % (
            g9.label, cid, ', '.join('%s -> %s' % k[:2] for k in kept7), '; '.join(k[2] for k in kept7)))
        import model_budget
        for n_, pg, parts in model_budget.model_pages(os.path.join(mod, 'ch.pac')):
            if pg > model_budget.LIMIT:
                note('attire %s: %d of %d pages - the game leaves the blood overlays out' % (n_, pg, model_budget.LIMIT))
        for e in extra:
            drop('attire %s (a mod holds 4)' % e)
    elif g9.has_chpac(cid):
        kept, extra = model_from_2008_family(g9.chpac(cid), cid, pac11, os.path.join(mod, 'ch.pac'), portrait, True)
        note('model: %s ch%d.pac attires %s converted (svr08_char)' % (g9.label, cid, ', '.join('%s -> %s' % k for k in kept)))
        for n in getattr(model_from_2008_family, 'notes', []):
            (warn if 'may crash' in n else note)(n)
        for e in extra:
            drop('attire %s (a mod holds 4)' % e)
    else:
        warn('%s has no model for %d: the base\'s is used' % (g9.label, cid))
    # ratings
    r = g9.g.dat[cid]
    if any(x not in (0, 50) for x in r[:7]):
        man['ratings'] = ','.join(str(max(0, x - 1)) for x in r[:7])
        note('ratings %s %s -> %s' % (g9.label, list(r[:8]), man['ratings']))
    if type(g9).__name__ == 'Game07':   # (another scale: fitted, tools/ratings_port.py)
        import ratings_port
        rr, rrep = ratings_port.map_ratings('svr2007', cid, g9)
        if rr:
            man['ratings'] = ','.join(map(str, rr))
        note(ratings_port.report_line(g9.label, rr, rrep))
    abilities_step(type(g9).__name__, g9, cid, man, note)
    # moves
    if not o.get('no_moves') and base != i11:
        moveset_step(g9, cid, base, o, work, mod, man, rep)
    # entrance
    progress(55, 'entrance')
    music, movie, ent, alt = g9.entrance_block(cid)
    if o.get('entrance'):
        man['entrance'] = o['entrance']
    else:
        e, via = entrance_from_names([g9.evp().get(ent), name, g9.short(cid)], g11)
        be = g11.entrance_block(base)[2]
        if not e and base != i11:
            e, via = (483 if r[208] else 522), 'generic (2011 has none of his)'
        if e and e != be:
            man['entrance'] = e
        note('entrance %s "%s" (%s)' % (man.get('entrance', be), g11.evp().get(man.get('entrance', be), '?'), via or 'the base\'s'))
    # theme, titantron
    progress(65, 'theme')
    recs = {} if g9.year == 7 else g9.music_records()
    if g9.year == 7:   # (XACT: the longest MUS_<id>_N cue)
        import xact
        th = xact.theme(g9.src.d, cid, os.path.join(mod, 'theme.mp3'), work)
        if th:
            man['song'] = 'theme.mp3'
            note('theme: XACT cue %s (%.0f s) -> theme.mp3' % th)
        else:
            warn('no %s theme (music id %d)' % (g9.label, cid))
    for m in ((music, cid) if g9.year != 7 else ()):
        if m not in (0, 255) and m * 100 in recs:
            src, gain = theme_from_record(recs[m * 100], os.path.join(mod, 'theme.mp3'), work)
            man['song'] = 'theme.mp3'
            note('theme: music yaf record %d -> theme.mp3 (%.1f LUFS %+.1f dB)' % (m * 100, src, gain))
            break
    else:
        if g9.year != 7:
            warn('no %s theme (music id %d)' % (g9.label, music))
    progress(85, 'titantron')
    for m in (movie, cid):
        p = g9.titantron(m) if m not in (0, 255) else None
        if p:
            shutil.copyfile(p, os.path.join(mod, 'movie.bik'))
            man['movie'] = 'movie.bik'
            note('titantron: movies/titantron/%s' % os.path.basename(p))
            break
    else:
        warn('no %s titantron (movie id %d)' % (g9.label, movie))
    # announcer, commentary
    class _NoNames:   # (the 2011 record of the same number may be someone else)
        def name(self, i): return g11.name(i11) if same11 else ''
    nm, clips = snd.announcer(S.ra_candidates(g9.g, _NoNames(), cid))
    if nm:
        man['announcer'] = nm
        note('announcer %s (2011 has his name calls)' % nm)
    else:
        warn('2011 has no ring-announcer recording of his name')
    if not (base == i11 and snd.commentary(i11)):
        man['call'] = o.get('call') if o.get('call') is not None else S.CALL.get(i11, 69 if r[208] else 77)
    if o.get('height'):
        man['height'] = '%.2f' % round(min(1.25, max(0.80, float(o['height']))), 2)
    progress(95, 'packing')
    out = os.path.join(outdir, mid + '.svrmod')
    txt = write_mod(mod, man, out, rep, pac11)
    log(txt)
    return out, mod


def build10(cid, outdir, o, rep):
    S = svr10()
    src = o['src']
    g10 = o['src_game']
    if cid not in g10.dat or not g10.named(cid):
        raise BadInput('SvR 2010 has no superstar %d' % cid)
    name = g10.name(cid)
    mid = re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')
    out = os.path.join(outdir, mid + '.svrmod')
    work = os.path.join(outdir, '.work_%d' % cid)
    if not src.folder:   # the files svr10_superstar and svr10_moves read
        progress(5, 'reading the disc')
        for rel in ('pac/ch/ch%d.pac' % cid, 'pac/misc.pac', 'pac/m.pac', 'pac/mpsp.pac', 'sound/music1.yaf',
                    'movies/titantron/%03d.bik' % cid):
            src.get(rel)
        src.get_dir('pac/evt', lambda n: n.lower().startswith('nyujyo'))
    bo = dict(pac10=o['src_game'].pac, pac11=o['pac11'], g10=g10, g11=o['g11'], snd=o['snd'], work=work,
              cache_work=os.path.join(outdir, '.work_cache'), base=o.get('base'), entrance=o.get('entrance'),
              call=o.get('call'), height=o.get('height'), no_moves=o.get('no_moves'),
              model={'11': '11', 'src': '10', None: '10'}.get(o.get('model')))
    progress(10, 'converting (model, renders, theme, moves)')
    with contextlib.redirect_stdout(sys.stderr):
        S.build(cid, out, bo)
    mod = os.path.join(outdir, mid)
    if os.path.exists(mod):
        shutil.rmtree(mod)
    shutil.copytree(os.path.join(work, 'mod'), mod)
    for line in open(os.path.join(work, 'build.log'), encoding='utf-8'):
        line = line.strip()
        if line.startswith(('warning', 'theme: none', 'titantron: none', 'renders: none', 'announcer: no')):
            rep['warnings'].append(line)
        elif line.startswith(('base =', 'model:', 'renders:', 'theme:', 'titantron:', 'entrance', 'moves:', 'announcer =', 'commentary', 'ratings')):
            rep['notes'].append(line)
    # attire names (CHANGE ATTIRE): 2010's own, for the attires its model pac gives
    names = attire_names(src, cid)
    man = collections.OrderedDict()
    for line in open(os.path.join(mod, 'manifest.txt'), encoding='utf-8', errors='replace'):
        if '=' in line:
            k, v = line.rstrip('\r\n').split('=', 1)
            man[k] = v
    if names and os.path.exists(os.path.join(mod, 'ch.pac')):
        digits = sorted(model_attires(os.path.join(mod, 'ch.pac')))
        added = []
        for a in digits:
            if a >= 1 and a in names and 'attire%d_name' % (a + 1) not in man:
                man['attire%d_name' % (a + 1)] = names[a][:31]
                added.append('%d %s' % (a + 1, names[a]))
        if added:
            rep['notes'].append('attire names from SvR 2010: ' + ', '.join(added))
    write_mod(mod, man, out, rep, o['pac11'])
    return out, mod


def model_attires(pac):
    """the attire digits of a model pac's wrestler (kind 2) entries"""
    d = open(pac, 'rb').read()
    _, groups = ch_tool.epk8_read(d)
    out = set()
    for typ, ents in groups:
        if typ == b'EMD ':
            for n, b in ents:
                s = n.decode('latin1').strip('\0 ')
                if len(s) == 8 and s[7] == '2' and s[6].isdigit():
                    out.add(int(s[6]))
    return out


def attire_names(src, cid):
    """{attire digit: name} from the game's COS table (misc.pac BATS/INIT child 5: "COS\\0", count,
    40-byte records {u32 id, u32 attires, 4 x {u32 name string id, s32 unlock}}) and string.pac
    entry "0000" child 3 (sdb: id -> text) - SvR 2010's layout, as 2011's"""
    try:
        g = svrfmt.epac_read(open(src.get('pac/misc.pac'), 'rb').read())[1]
        init = ch_tool.unpack(dict(((ty, n), b) for ty, es in g for n, b in es)[(b'BATS', b'INIT')])
        c5 = ch_tool.unpack(dict(svrfmt.pach_read(init))[5])
        if c5[:4] != b'COS\0':
            return {}
        recs = {}
        for k in range(struct.unpack_from('<I', c5, 4)[0]):
            r = c5[8 + 40 * k: 8 + 40 * (k + 1)]
            i, cnt = struct.unpack_from('<II', r, 0)
            recs[i] = [struct.unpack_from('<I', r, 8 + 8 * a)[0] for a in range(min(cnt, 4))]
        if cid not in recs:
            return {}
        g = svrfmt.epac_read(open(src.get('pac/string.pac'), 'rb').read())[1]
        table = ch_tool.unpack(dict(((ty, n), b) for ty, es in g for n, b in es)[(b'SDB ', b'0000')])
        sdb = ch_tool.unpack(dict(svrfmt.pach_read(table))[3])
        text = {}
        for k in range(struct.unpack_from('<I', sdb, 4)[0]):
            off, ln, i, z = struct.unpack_from('<IIII', sdb, 8 + 16 * k)
            text[i] = sdb[off:off + ln].split(b'\0')[0].decode('latin1')
        return {a: text[nid] for a, nid in enumerate(recs[cid]) if nid in text}
    except (KeyError, OSError, struct.error, IndexError):
        return {}


# --------------------------------------------------------------------------- WWE '12 / '13 / 2K14

class GameW:
    """WWE '12 (docs/OTHER_GAMES.md): pac/ch/chEtc.pac CHAR/DAT (290 B: name +0x2E, scale +40, gender +238,
    weight class +239, selectable +251; 12 ratings - not 2011's 7: left to the base), CHAR/PRO (1072 B,
    entrance block +0x1D4: music, movie, entrance, alt); models pac/ch/chNNN.pac (WWE '13's rig and
    shaders in an EPK8: w13_char.py); renders menu/menuHD.pac SSFA / SSFB / SSFC (2011's format);
    themes sound/Music.pck Play_MUS_<music>_0_0 (Wwise Vorbis: wwise.py + the player's vgmstream);
    entrances and titantrons through entrance_port.py. Superstar ids are 2011's for 100-292."""

    year = 12
    label = "WWE '12"
    suffix = '_12'
    game_id = 'wwe12'
    dat_name, dat_scale, dat_gender, dat_class, pro_entrance = 0x2E, 40, 238, 239, 0x1D4
    # 2011's 7 ratings (DAT +0..+6, display - 1) from '12's 12 (DAT +4..+15, the same scale): (offset, minus),
    # each the '12 value closest to 2011's over the 117 superstars both games have (mean error 1.6-2.6)
    rating_map = ((5, 0), (8, 0), (9, 0), (12, 0), (7, 0), (14, 0), (13, -5))
    no_ratings = ''

    def __init__(self, src, svrmod=None):
        import moves_port as MP
        self.MP, self.src, self.svrmod = MP, src, svrmod
        g = MP.read_pac(self.pac_bytes('pac/ch/chEtc.pac'))
        ch = {(t, n): b for t, es in g for n, b in es}

        def pach(b):
            return {i: MP.unz(d) for i, d in svrfmt.pach_read(MP.unz(b))}
        self.raw_dat = pach(ch[(b'CHAR', b'DAT')])
        self.pro = pach(ch[(b'CHAR', b'PRO')])
        self.dat = {}
        for i, r in self.raw_dat.items():   # (laid out as 2011's for the shared helpers)
            x = bytearray(264)
            nm = r[self.dat_name:self.dat_name + 32].split(b'\0')[0][:31]
            x[34:34 + len(nm)] = nm
            x[170:170 + len(nm)] = nm
            x[28:30] = r[self.dat_scale:self.dat_scale + 2]
            x[208], x[209] = r[self.dat_gender], r[self.dat_class]
            self.dat[i] = bytes(x)
        self._ssf = None

    def pac_bytes(self, rel):
        return open(self.src.get(rel), 'rb').read()

    def name(self, i):
        r = self.dat.get(i)
        return r[34:66].split(b'\0')[0].decode('latin1').strip() if r else ''

    def short(self, i):
        return self.name(i)

    def named(self, i):
        return self.name(i) not in ('', '0')

    def id11(self, i):
        return i

    def chpac_rel(self, i):
        return 'pac/ch/ch%d.pac' % i

    def has_chpac(self, i):
        return self.src.has(self.chpac_rel(i))

    def ssf(self):
        if self._ssf is None:
            self._ssf = {}
            for t, es in self.MP.read_pac(self.pac_bytes('pac/menu/menuHD.pac')):
                if t in (b'SSFA', b'SSFB', b'SSFC'):
                    for n, b in es:
                        if n.isdigit():
                            self._ssf[(t.decode(), int(n))] = b
        return self._ssf

    def entrance_block(self, i):
        p = self.pro.get(i)
        return struct.unpack_from('<4H', p, self.pro_entrance) if p and len(p) >= self.pro_entrance + 8 else (i, i, i, i)

    def music_pck(self):
        return self.src.get('sound/Music.pck')

    def theme_events(self, music, cid):
        return ['Play_MUS_%04d_0_0' % m for m in (music, cid) if m]

    def roster(self):
        return [c for c in sorted(self.dat) if 100 <= c < 400 and self.named(c) and self.has_chpac(c)
                and not self.name(c).upper().startswith(('MOTION', 'TEST', 'DUMMY'))]


class GameW13(GameW):
    """WWE '13: as WWE '12, in xcompressed pacs (svrmod xdec); chEtc CHAR/DAT 324 B (name +58, scale +52,
    gender +256, weight class +292 [guess: 102 of 138 shared superstars agree]), CHAR/PRO 1180 B with
    the entrance block at +576 (music, movie, entrance); themes Play_MUS_<music>_0_0; titantrons are
    Bink 2 (entrance_port --titantron: bink2_convert with the player's decoder, opt in)."""

    year = 13
    label = "WWE '13"
    suffix = '_13'
    game_id = 'wwe13'
    dat_name, dat_scale, dat_gender, dat_class, pro_entrance = 58, 52, 256, 292, 576
    rating_map = None
    no_ratings = "WWE '13's ratings aren't in its DAT / PRO records (not found yet)"

    def __init__(self, src, svrmod=None):
        import wwe13_entrance as WE
        self.WE = WE
        GameW.__init__(self, src, WE.find_svrmod(svrmod))

    def pac_bytes(self, rel):
        raw = open(self.src.get(rel), 'rb').read()
        return self.WE.xdec(raw, self.svrmod) if raw[:4] == b'\x0f\xf5\x12\xed' else raw


class GameW2K14(GameW13):
    """WWE 2K14: xcompressed like '13; chEtc CHAR/DAT 208 B (name string id u16 +10 -> string.pac SDB table
    5000 child 0, English; height +22 in 2011's units; weight class +26; gender +27, 1 = diva), CHAR/PRO
    1276 B with the entrance block at +576 as '13; model pacs say EPAC with 16-byte entries, several
    attires (Cena 10), each a wrestler model (kind 2) and a second one (kind 4) - the first four kind 2
    become the 2011 attires; renders menu/menuHD.pac in 2011's format. Themes: the events
    Play_MUS_<music>_0_0 as '12 / '13, in sound/Game_Sound_E_S.pck; their streams in sound/Game_Sound_M.pck
    (wwise.theme stream_pck)."""

    year = 14
    label = 'WWE 2K14'
    suffix = '_2k14'
    game_id = 'wwe2k14'
    dat_name, dat_scale, dat_gender, dat_class, pro_entrance = 10, 22, 27, 26, 576
    dat_size = 208
    no_ratings = ("2K14's 14 ratings (DAT +36..+49) match 2011's only loosely - it re-rated people - so they "
                  "aren't mapped")
    max_attires = 4

    def __init__(self, src, svrmod=None):
        GameW13.__init__(self, src, svrmod)
        names = self._names()
        for i, r in self.raw_dat.items():   # (the name is a string id: rebuild the 2011-shaped record's name)
            if len(r) < 28:
                continue
            nm = names.get(struct.unpack_from('<H', r, self.dat_name)[0], '').encode('latin1', 'replace')[:31]
            x = bytearray(self.dat[i])
            x[34:66] = bytes(32)
            x[170:202] = bytes(32)
            x[34:34 + len(nm)] = nm
            x[170:170 + len(nm)] = nm
            self.dat[i] = bytes(x)

    def _names(self):
        """string.pac SDB 5000 (English) child 0: {u32 0, u32 count, count x {offset, length, id}} -> {id: text}"""
        g = self.MP.read_pac(self.pac_bytes('pac/string.pac'))
        es = {n: b for t, ents in g for n, b in ents}
        if b'5000' not in es:
            return {}
        u = self.MP.unz(dict(svrfmt.pach_read(self.MP.unz(es[b'5000'])))[0])
        out = {}
        for k in range(struct.unpack_from('<I', u, 4)[0]):
            off, ln, sid = struct.unpack_from('<3I', u, 8 + 12 * k)
            out[sid] = u[off:off + ln].split(b'\0')[0].decode('utf-8', 'replace')
        return out

    def music_pck(self):
        return self.src.get('sound/Game_Sound_E_S.pck')

    def stream_pck(self):
        return self.src.get('sound/Game_Sound_M.pck')


class GameW2K15(GameW2K14):
    """WWE 2K15: as 2K14 (DAT 208 B with the same fields, names in string.pac SDB 5000, themes in
    Game_Sound_E_S / M.pck) except: model pacs are xcompressed EPK8s; CHAR/PRO is 1252 / 1332 B with the
    entrance block at +576 as u32s (music, movie, entrance, alt): music and movie are id * 100 + variant
    (10400 = Play_MUS_0104_0_0, movies/titantron/ent_0104_0_0.bk2; 0 / 299900 = generic); renders are
    menu/menuHD2.pac SFAG / SFBG / SFCG (512 / 512 / 256): BPE'd raw 32-bit pixels after a 44-byte header
    (side / 256 as BE u16 at +12), bytes R A B G (ARGB 16-bit swapped), bottom row first - made into DXT5 DDS here."""

    year = 15
    label = 'WWE 2K15'
    suffix = '_2k15'
    game_id = 'wwe2k15'
    no_ratings = "2K15's ratings (DAT +36..+49, 2K14's layout) aren't mapped"

    def entrance_block(self, i):
        p = self.pro.get(i)
        return struct.unpack_from('<4I', p, self.pro_entrance) if p and len(p) >= self.pro_entrance + 16 else (0, 0, i, i)

    def theme_events(self, music, cid):
        ev = ['Play_MUS_%04d_%d_0' % (music // 100, music % 100)] if 100 <= music < 100000 else []
        return ev + ['Play_MUS_%04d_0_0' % cid]

    def ssf(self):
        if self._ssf is None:
            self._ssf = _RawRenders()
            names = {b'SFAG': 'SSFA', b'SFBG': 'SSFB', b'SFCG': 'SSFC'}
            for t, es in self.MP.read_pac(self.pac_bytes('pac/menu/menuHD2.pac')):
                if t in names:
                    for n, b in es:
                        if n.isdigit():
                            dict.__setitem__(self._ssf, (names[t], int(n)), b)
        return self._ssf


def epac20(raw):
    """2K16+ model pacs: EPAC groups {4CC, u16 count field (entries * 4), u16 kind, u32 0} of 20-byte entries
    {name[4], u32 sector, u32 size / 0x100, u32 size2 / 0x100, u32} -> [(type, [(name, data)])]"""
    out, p = [], 0x800
    while p < 0x4000 and raw[p:p + 4].strip(b'\0'):
        typ, cnt = raw[p:p + 4], struct.unpack_from('<H', raw, p + 4)[0]
        p += 12
        ents = []
        for _ in range(cnt // 4):
            sec, sz = struct.unpack_from('<II', raw, p + 4)
            ents.append((raw[p:p + 4], raw[0x4000 + sec * 0x800:0x4000 + sec * 0x800 + sz * 0x100]))
            p += 20
        out.append((typ, ents))
    return out


class GameW2K16(GameW2K15):
    """WWE 2K16: chEtc / string.pac as 2K15 (xcompressed, 12-byte tables) but CHAR/DAT 216 B in records 0-309
    with the superstar id u16 at +4 (ids up to 630) and 2K15's fields 4 bytes on (name string id +14, height
    +26, weight class +30, gender +31); CHAR/PRO 1348 B, entrance block +588 = 4 x u32 (music id * 1000 +
    variant, movie id * 100 + variant, entrance, alt). Model pacs: EPAC with 20-byte entries all named "0001"
    (epac20), no kind in the name: an entry with 0x5208.. children is the entrance-gear model (2K14's kind 4,
    left out), the rest are attires in file order (exact copies dropped); hair at 0x271a (w13_char takes it).
    Renders menu/menuHD2.pac SFAG / SFCG (2K15's raw format; no full body). Sounds in lowercase pcks; their
    Wwise banks are v113."""

    year = 16
    label = 'WWE 2K16'
    suffix = '_2k16'
    game_id = 'wwe2k16'
    dat_name, dat_scale, dat_gender, dat_class, pro_entrance = 14, 26, 31, 30, 588
    dat_size = 216
    no_ratings = "2K16's ratings aren't mapped"

    def __init__(self, src, svrmod=None):
        GameW2K15.__init__(self, src, svrmod)
        ids = {i: struct.unpack_from('<H', r, 4)[0] for i, r in self.raw_dat.items() if len(r) >= 6}
        self.raw_dat = {ids[i]: r for i, r in self.raw_dat.items() if i in ids}
        self.dat = {ids[i]: r for i, r in self.dat.items() if i in ids}
        self.pro = {ids[i]: r for i, r in self.pro.items() if i in ids}

    def theme_events(self, music, cid):
        ev = ['Play_MUS_%04d_%02d_0_0' % (music // 1000, music % 1000)] if 1000 <= music < 1000000 else []
        return ev + ['Play_MUS_%04d_00_0_0' % cid]

    def music_pck(self):
        return self.src.get('sound/game_sound_e_s.pck')

    def stream_pck(self):
        return self.src.get('sound/game_sound_m.pck')

    def roster(self):
        return [c for c in sorted(self.dat) if 100 <= c < 1000 and self.named(c) and self.has_chpac(c)
                and not self.name(c).upper().startswith(('MOTION', 'TEST', 'DUMMY'))]

    def model_entries(self, raw):
        return attires(b for t, es in epac20(raw) if t == b'EMD ' for n, b in es)


def attires(pachs):
    """2K16+ model PACHs (no kind in a name) -> [(EPK8 name, PACH)]: the entrance-gear model (0x5208..
    children) and exact copies left out"""
    out, seen = [], set()
    for b in pachs:
        kids = dict(svrfmt.pach_read(ch_tool.unpack(b)))
        if 0x5208 in kids or 0 not in kids:
            continue
        key = ch_tool.unpack(kids[0])
        if key in seen:
            continue
        seen.add(key)
        out.append((b'%04d0402' % (len(out) + 1), b))
    return out


def hspc(raw):
    """2K17 HSPC: u32 index bytes +0x18, count +0x38; index at 0x800 of 20-byte {u64 name hash, u32 0, u32
    location offset, i32 -1}; locations at 0x800 + the index rounded to 0x800, {u32 sector, u32 size / 0x100,
    u32 0xC0000} -> [(hash, bytes)] in storage order (the names are hashed: the hash isn't known)"""
    h = struct.unpack_from('<16I', raw, 0)
    loc = 0x800 + ((h[6] + 0x7FF) & ~0x7FF)
    out = []
    for i in range(h[14]):
        hs, _, lo, _ = struct.unpack_from('<QIIi', raw, 0x800 + 20 * i)
        sec, sz, _ = struct.unpack_from('<3I', raw, loc + lo)
        out.append((sec, hs, raw[sec * 0x800:sec * 0x800 + sz * 0x100]))
    return [(hs, b) for _, hs, b in sorted(out)]


def shdc(b):
    """2K17 SHDC (a PACH's place): u32 count +0x38, table at u32 +0x1C of {u32 child id, u32 offset from the
    SHDC, u32 size, u32 0} -> [(child id, bytes)]"""
    n, t = struct.unpack_from('<I', b, 0x38)[0], struct.unpack_from('<I', b, 0x1C)[0]
    out = []
    for i in range(n):
        cid, off, sz, _ = struct.unpack_from('<4I', b, t + 16 * i)
        out.append((cid, b[off:off + sz]))
    return out


class GameW2K17(GameW2K16):
    """WWE 2K17: pacs are xcompressed HSPCs of SHDCs (hspc / shdc; entry names hashed). chEtc: the SHDC of
    220-byte leaves is CHAR/DAT (leaf = record, id u16 +4, name string id +14, height +30, weight class +34,
    gender +35: 2K16's with 4 bytes at +24 - 4000 + the id, a picture number), the one of 1348-byte leaves
    CHAR/PRO (2K16's: entrance block +588). Names: string_360.pac, five language SHDCs (English first) of one
    ZLIB leaf with 2K14's string table. Models: chNNN.pac's SHDCs in storage order (2K16's attire rules).
    Themes as 2K16 (Play_MUS_0104_00_0_0). Renders: menu/SSFaceHD.pac holds 176 + 176 of them in 2K15's raw
    format, but under hashed names in an order that isn't the ids' - not mapped yet, so none are taken."""

    year = 17
    label = 'WWE 2K17'
    suffix = '_2k17'
    game_id = 'wwe2k17'
    dat_name, dat_scale, dat_gender, dat_class, pro_entrance = 14, 30, 35, 34, 588
    dat_size = 220
    no_ratings = "2K17's ratings aren't mapped"

    def __init__(self, src, svrmod=None):
        import moves_port as MP
        import wwe13_entrance as WE
        self.MP, self.WE, self.src = MP, WE, src
        self.svrmod = WE.find_svrmod(svrmod)
        sets = [[(i, ch_tool.unpack(x)) for i, x in shdc(b)] for _, b in hspc(self.pac_bytes('pac/ch/chEtc.pac'))
                if b[:4] == b'SHDC']
        dat = max((s for s in sets if s and len(s[0][1]) == self.dat_size), key=len)
        pro = max((s for s in sets if s and len(s[0][1]) == 1348), key=len)
        ids = {i: struct.unpack_from('<H', r, 4)[0] for i, r in dat}
        self.raw_dat = {ids[i]: r for i, r in dat}
        self.pro = {ids[i]: r for i, r in pro if i in ids}
        names = self._names()
        self.dat = {}
        for c, r in self.raw_dat.items():   # (laid out as 2011's for the shared helpers)
            x = bytearray(264)
            nm = names.get(struct.unpack_from('<H', r, self.dat_name)[0], '').encode('latin1', 'replace')[:31]
            x[34:34 + len(nm)] = nm
            x[170:170 + len(nm)] = nm
            x[28:30] = r[self.dat_scale:self.dat_scale + 2]
            x[208], x[209] = r[self.dat_gender], r[self.dat_class]
            self.dat[c] = bytes(x)
        self._ssf = _RawRenders()
        self.theme_missing = 'no theme events'

    def _names(self):
        for _, b in hspc(self.pac_bytes('pac/string_360.pac')):
            if b[:4] != b'SHDC':
                continue
            u = self.MP.unz(shdc(b)[0][1])
            if len(u) < 8 or struct.unpack_from('<I', u, 0)[0] != 0:
                continue
            out = {}
            for k in range(struct.unpack_from('<I', u, 4)[0]):
                off, ln, sid = struct.unpack_from('<3I', u, 8 + 12 * k)
                out[sid] = u[off:off + ln].split(b'\0')[0].decode('utf-8', 'replace')
            return out   # (the first: English)
        return {}

    def ssf(self):
        return self._ssf

    def pictures(self):
        """SSFaceHD.pac's renders -> [(key, 512 render, 256 render)] (raw, BPE'd) in storage order: the big
        ones first, then the small ones in the same order (a pair = one superstar); key = the big one's name
        hash in hex"""
        if not hasattr(self, '_pics'):
            big, small = [], []
            for hs, b in hspc(self.pac_bytes('pac/menu/SSFaceHD.pac')):
                if b[:4] == b'BPE ':
                    side = math.isqrt((len(ch_tool.unpack(b)) - 44) // 4)
                    (big if side == 512 else small if side == 256 else []).append((hs, b))
            self._pics = [('%016x' % hb, b, s_) for (hb, b), (_, s_) in zip(big, small)]
        return self._pics

    def pick_render(self, cid, key):
        """--picture <key>: that picture pair of pictures() as this superstar's renders"""
        pics = {k: (b, s_) for k, b, s_ in self.pictures()}
        if key.lower() not in pics:
            raise BadInput('--picture %s: not one of %s\'s %d pictures (see pictures)' % (key, self.label, len(pics)))
        dict.__setitem__(self._ssf, ('SSFA', cid), pics[key.lower()][0])
        dict.__setitem__(self._ssf, ('SSFC', cid), pics[key.lower()][1])

    def model_entries(self, raw):
        return attires(svrfmt.pach_write(shdc(b)) for _, b in hspc(raw) if b[:4] == b'SHDC')


def raw_image(v):
    """a 2K15+ raw render (BPE'd or not): 44-byte header (+12: BE u16 side / 256), then R A B G pixels,
    bottom row first -> a PIL RGBA image"""
    from PIL import Image
    u = ch_tool.unpack(v)
    side = math.isqrt((len(u) - 44) // 4)
    px = u[44:44 + side * side * 4]
    if side < 16 or struct.unpack_from('>H', u, 12)[0] * 256 != side:
        raise ValueError('raw render: %d bytes for side %d' % (len(u), side))
    q = bytearray(len(px))
    q[0::4], q[1::4], q[2::4], q[3::4] = px[0::4], px[3::4], px[2::4], px[1::4]   # R A B G -> R G B A
    return Image.frombytes('RGBA', (side, side), bytes(q)).transpose(Image.FLIP_TOP_BOTTOM)


class _RawRenders(dict):
    """2K15 / 2K16 menuHD2 renders, made into DDS when read"""

    def __getitem__(self, k):
        v = dict.__getitem__(self, k)
        if v[:4] == b'DDS ':
            return v
        import svr08_char
        v = svr08_char.dds_encode(raw_image(v), 'DXT5')
        dict.__setitem__(self, k, v)
        return v


def model_w(g, cid, pac11, out, portrait):
    """w13_char on the (decoded) EPK8 model pac -> [entries], notes"""
    import w13_char
    import tempfile
    raw = g.pac_bytes(g.chpac_rel(cid))
    tmp = os.path.join(tempfile.gettempdir(), 'svr_w_ch%d.pac' % cid)
    left = []
    if getattr(g, 'max_attires', 0):   # (2K14: wrestler models only, the first four attires, as an EPK8)
        if hasattr(g, 'model_entries'):
            ents = g.model_entries(raw)
        else:
            ents = [(n, b) for t, es in g.MP.read_pac(raw) if t == b'EMD ' for n, b in es if n.endswith(b'2')]
            ents.sort(key=lambda e: e[0])
        left = [n.decode('latin1') for n, _ in ents[g.max_attires:]]
        w13_char.write_epk8(os.path.join(pac11, 'ch', 'ch104.pac'), [(b'EMD ', n, b) for n, b in ents[:g.max_attires]],
                            tmp)
        raw = open(tmp, 'rb').read()
    open(tmp, 'wb').write(raw)
    ptmp = None
    if portrait:   # (w13_char takes the portrait as a file)
        ptmp = tmp + '.portrait.dds'
        open(ptmp, 'wb').write(portrait)
    with contextlib.redirect_stdout(sys.stderr):
        w13_char.convert(tmp, out, os.path.join(pac11, 'ch', 'ch104.pac'), ptmp)
    for f in (tmp, ptmp):
        try:
            if f:
                os.remove(f)
        except OSError:
            pass
    return left


def fit_budget(path, tmpl):
    """keep every attire of the converted ch.pac at `path` within the match's 192 pages (model_budget) once the
    game leaves its blood overlays out (0x7530-0x7532; the game does that itself): over it, the eyelashes
    (0x4e20) go; still over, the attire goes (the first one stays) - the game leaves a model over the
    budget out altogether. -> [what was taken out]"""
    import model_budget
    import w13_char
    raw = open(path, 'rb').read()
    _, groups = ch_tool.epk8_read(raw)
    ents, out, changed = [], [], False
    for typ, es in groups:
        for name, blob in es:
            if not name.strip(b'\0'):
                continue
            kids = {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(blob))}

            def over():
                return model_budget.attire_pages({k: v for k, v in kids.items() if k not in (0x7530, 0x7531, 0x7532)})[0] \
                    > model_budget.LIMIT
            n = name.decode('latin1').strip('\0 ')
            if over() and 0x4e20 in kids:
                del kids[0x4e20]
                blob = w13_char.pack_kids(kids)
                changed = True
                out.append('attire %s: its eyelashes (over the %d-page model budget with them)' % (n, model_budget.LIMIT))
            if over() and ents:
                changed = True
                out.append('attire %s: over the %d-page model budget even without eyelashes and blood' % (n, model_budget.LIMIT))
                continue
            if over():   # (the first attire: kept, but the game will leave the model out - the model needs fewer vertices)
                out.append('attire %s: STILL over the %d-page model budget - the game will leave this superstar out '
                           '(the model needs fewer vertices / triangles)' % (n, model_budget.LIMIT))
            ents.append((typ, name, blob))
    if changed:
        w13_char.write_epk8(tmpl, ents, path)
    return out


def moveset_step(gw, cid, base, o, work, mod, man, rep):
    """the superstar's own moveset (moveset_port.py): moves.txt over the base's profile, ported moves
    as a move pack in the mod's moves/ folder"""
    import moveset_port
    import moveset_maps
    import moves_port as MP
    note, warn, drop = rep['notes'].append, rep['warnings'].append, rep['dropped'].append
    gid = o['game']
    pro = moveset_maps.profiles(gw)
    if cid not in pro:
        drop('moveset: the base\'s (no %s profile for %d)' % (gw.label, cid))
        return
    mg = MP.GAME_CLASS.get(gid)
    if mg is not None:
        mgame = mg(o['src']) if gid == 'wwe12' else mg(o['src'], getattr(gw, 'svrmod', None))
        waze = mgame.waze
    elif gid in MP.OLD:   # (SvR 2007-2009: 2011's ids; the tracks copied - moves_port.Game0789)
        mgame = MP.Game0789(o['src'], os.path.join(work, '.m0789'))
        waze = mgame.waze
    else:
        mgame, waze = None, moveset_maps.waze_old(o['src'])
    c = moveset_port.Ctx(gid=gid, cid=cid, base=base, g11=o['g11'], pro=pro[cid], waze=waze, mgame=mgame,
                         pac11=o['pac11'], svrmod=getattr(gw, 'svrmod', None), work=work, mod=mod, rep=rep,
                         port=not o.get('no_port'), max_port=o.get('max_port', 40),
                         port_taunts=not o.get('no_taunts'),
                         same_ids=gid in moveset_maps.SAME_IDS)
    res = moveset_port.port_moveset(c)
    if res is None:
        drop('moveset: the base\'s (no %s moveset map yet)' % gw.label)
        return
    if res['pack']:
        moveset_port.merge_pack(res['pack'], mod)
    if res['moves_txt']:
        open(os.path.join(mod, 'moves.txt'), 'w', newline='\n').write('\n'.join(res['moves_txt']) + '\n')
        man['moves'] = 'moves.txt'
    with open(os.path.join(work, 'moves_table.tsv'), 'w', encoding='utf-8') as f:
        f.write('src_off\toff2011\tsrc_id\tname\tverdict\tid_in_slot\n')
        for so, to, v, nm, verdict, nid in res['rows']:
            f.write('%s\t0x%03x\t%d\t%s\t%s\t%d\n' % ('' if so is None else '0x%03x' % so, to, v, nm, verdict, nid))
    note('moveset: %d slots over the base (%s); table in .work/moves_table.tsv' % (len(res['moves_txt']),
                                                                                   moveset_port.summary(res)))
    kept = [r for r in res['rows'] if r[4].startswith('base kept')]
    if kept:
        drop('moveset: %d slots keep the base\'s move (see moves_table.tsv)' % len(kept))


def buildW(cid, outdir, o, rep):
    import subprocess
    S = svr10()
    gw, g11, snd, pac11 = o['src_game'], o['g11'], o['snd'], o['pac11']
    if cid not in gw.dat or not gw.named(cid):
        raise BadInput('%s has no superstar %d' % (gw.label, cid))
    name = gw.name(cid).title() if gw.name(cid).isupper() else gw.name(cid)
    mid = re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_') + gw.suffix
    work = os.path.join(outdir, '.work_%d' % cid)
    mod = os.path.join(outdir, mid)
    for d in (work, mod):
        if os.path.exists(d):
            shutil.rmtree(d)
        os.makedirs(d)
    note, warn, drop = rep['notes'].append, rep['warnings'].append, rep['dropped'].append
    progress(5, 'base superstar')
    i11 = gw.id11(cid)
    same11 = i11 in playable11(g11) and S.norm(g11.name(i11)) == S.norm(gw.name(cid))
    if o.get('base'):
        base, why = o['base'], 'option'
    elif same11:
        base, why = i11, '2011 has him: his own moves and commentary'
    else:
        base, why = choose_base_old(type('G', (), {'g': type('D', (), {'dat': gw.dat})})(), g11, cid)
    note('base %d %s (%s)' % (base, g11.name(base), why))
    man = collections.OrderedDict(type='superstar', id=mid, name=name[:31], short=name[:31], base=base,
                                  author=gw.label + ' port', made_with='Port tools (%s)' % gw.label, version='1.0')
    progress(12, 'renders')
    ssf = gw.ssf()
    portrait = None
    if ('SSFC', cid) in ssf:
        portrait = ch_tool.unpack(ssf[('SSFC', cid)])
        open(os.path.join(mod, 'render_small.dds'), 'wb').write(portrait)
    for grp, fn in (('SSFA', 'render.dds'), ('SSFB', 'render_full.dds')):
        if (grp, cid) in ssf:
            open(os.path.join(mod, fn), 'wb').write(pad_render(ch_tool.unpack(ssf[(grp, cid)])))
    note('renders: %s menuHD SSFA / SSFB / SSFC %d' % (gw.label, cid) if ('SSFA', cid) in ssf else
         'no %s render: the base\'s%s' % (gw.label, ' (its renders are unnamed: pick one with pictures + --picture)'
                                          if hasattr(gw, 'pictures') else ''))
    progress(20, 'model')
    if gw.has_chpac(cid):
        left = model_w(gw, cid, pac11, os.path.join(mod, 'ch.pac'), portrait)
        if left:
            drop('attires past %d (2011 has 4 at most): %s' % (gw.max_attires, ', '.join(left)))
        import model_budget
        pages = model_budget.model_pages(os.path.join(mod, 'ch.pac'))
        note('model: %s ch%d.pac, %d attires (w13_char)' % (gw.label, cid, len(pages)))
        for n_, pg, parts in pages:
            if pg > model_budget.LIMIT:
                note('attire %s: %d of %d pages - the game leaves the blood overlays out' % (n_, pg, model_budget.LIMIT))
    else:
        warn('%s has no model for %d: the base\'s is used' % (gw.label, cid))
    import ratings_port   # (fitted on the superstars each game shares with 2011)
    rr, rrep = ratings_port.map_ratings(gw.game_id, cid, gw)
    if rr:
        man['ratings'] = ','.join(map(str, rr))
        note(ratings_port.report_line(gw.label, rr, rrep))
    else:
        drop('ratings (%s): the base\'s' % (rrep['note'] or gw.no_ratings))
    abilities_step(gw.game_id, gw, cid, man, note)   # (every game: its own list, else guessed from the ratings)
    music, movie, ent, alt = gw.entrance_block(cid)
    # theme
    progress(45, 'theme')
    import wwise
    events = gw.theme_events(music, cid)
    try:
        ev, why = (wwise.theme(gw.music_pck(), events, os.path.join(mod, 'theme.mp3'), work, o.get('vgmstream'),
                               stream_pck=gw.stream_pck() if hasattr(gw, 'stream_pck') else None)
                   if events and gw.music_pck() else (None, getattr(gw, 'theme_missing', 'no theme events')))
    except Exception as e:   # noqa: BLE001
        ev, why = None, str(e)[:160]
    if ev:
        man['song'] = 'theme.mp3'
        note('theme: %s (%s) -> theme.mp3' % (ev, why))
    else:
        warn('theme: %s' % why)
    # entrance + titantron (entrance_port)
    progress(60, 'entrance')
    if o.get('entrance'):
        man['entrance'] = o['entrance']
    else:
        try:
            fr = subprocess.run([sys.executable, os.path.join(HERE, 'entrance_port.py'), 'free-numbers', '--game-pac', pac11,
                                 '--mods', os.path.join(os.path.dirname(pac11), 'Mods')], capture_output=True, text=True)
            free = json.loads(fr.stdout or '[]')
            free = free if isinstance(free, list) else free.get('free', [])
        except Exception:   # noqa: BLE001
            free = []
        number = i11 if i11 in free else (free[0] if free else None)
        if number:
            cmd = [sys.executable, os.path.join(HERE, 'entrance_port.py'), 'convert', '--game', gw.game_id,
                   '--src', o['src'].folder or o['src'].d.path, '--entrance', str(ent), '--number', str(number),
                   '--out', os.path.join(work, 'entrance'), '--name', name, '--game-pac', pac11, '--titantron', str(movie)]
            if gw.svrmod:
                cmd += ['--svrmod', gw.svrmod]
            if o.get('bink2_tool'):
                cmd += ['--bink2-tool', o['bink2_tool']]
            r = subprocess.run(cmd, capture_output=True, text=True)
            try:
                er = json.loads(r.stdout.strip().splitlines()[-1]) if r.stdout.strip() else {}
            except Exception:   # noqa: BLE001
                er = {}
            pk = os.path.join(work, 'entrance', 'pack')
            if r.returncode == 0 and os.path.exists(os.path.join(pk, 'pack.txt')):
                shutil.copytree(pk, os.path.join(mod, 'moves'), dirs_exist_ok=True)
                man['entrance'] = number
                note('entrance: %s %d -> 2011 number %d (entrance_port)' % (gw.label, ent, number))
                for f in ('movie.bik', 'movie.mp4'):
                    if os.path.exists(os.path.join(work, 'entrance', f)):
                        shutil.copyfile(os.path.join(work, 'entrance', f), os.path.join(mod, f))
                        man['movie'] = f
                        note('titantron: %s %d -> %s' % (gw.label, movie, f))
                        break
                rep['warnings'] += ['entrance: ' + w for w in er.get('warnings', [])]
            else:
                warn('entrance %d not converted (%s)' % (ent, (er.get('error') or r.stderr.strip()[-160:] or '?')))
        if 'entrance' not in man:
            e, via = entrance_from_names([name], g11)
            if e:
                man['entrance'] = e
                note('entrance %d "%s" (2011 has his)' % (e, g11.evp().get(e, '?')))
    if 'movie' not in man:
        warn('no titantron (movie %d)' % movie)
    if not getattr(gw, 'moveset_map', True):
        drop('moveset: the base\'s (no %s moveset map yet)' % gw.label)
    elif not o.get('no_moves') and base != i11:
        progress(70, 'moveset')
        moveset_step(gw, cid, base, o, work, mod, man, rep)
    progress(85, 'announcer')

    class _Names:
        def name(self, i): return g11.name(i11) if same11 else ''
    nm, clips = snd.announcer(S.ra_candidates(type('G', (), {'name': lambda s, i: name, 'short': lambda s, i: name})(),
                                              _Names(), cid))
    if nm:
        man['announcer'] = nm
        note('announcer %s' % nm)
    if not (base == i11 and snd.commentary(i11)):
        man['call'] = o.get('call') if o.get('call') is not None else S.CALL.get(i11, 69 if gw.dat[cid][208] else 77)
    progress(95, 'packing')
    out = os.path.join(outdir, mid + '.svrmod')
    write_mod(mod, man, out, rep, pac11)
    return out, mod


# --------------------------------------------------------------------------- commands

def open_game(game, src, svrmod=None):
    if game == 'svr2010':
        S = svr10()
        pac = src.get_dir('pac/ch', lambda n: n.lower() == 'chetc.pac').rsplit(os.sep, 1)[0]
        src.get('pac/DLC_HD.pac')
        g = S.Game(pac, 10)
        g.roster = lambda: [c for c in sorted(g.dat) if 100 <= c < 400 and g.named(c)
                            and not g.name(c).upper().startswith(('MOTION', 'MOVES MAN'))]
        g.has_chpac = lambda i: src.has('pac/ch/ch%d.pac' % i)
        g.ssf_keys = lambda: g.ssf
        g.render_ids = {i for (t, i) in g.ssf if t == 'SSFB'}
        g.entrance_block_ = g.entrance_block
        return g
    if game in ('svr2009', 'svr2008', 'svr2007'):
        g = {'svr2009': Game09, 'svr2008': Game08, 'svr2007': Game07}[game](src)
        g.render_ids = {i for (t, i) in g.ssf() if t == 'SSFB'}
        g.dat = g.g.dat
        return g
    if game in ('wwe12', 'wwe13', 'wwe2k14', 'wwe2k15', 'wwe2k16', 'wwe2k17'):
        g = {'wwe12': GameW, 'wwe13': GameW13, 'wwe2k14': GameW2K14, 'wwe2k15': GameW2K15, 'wwe2k16': GameW2K16,
             'wwe2k17': GameW2K17}[game]
        g = g(src) if game == 'wwe12' else g(src, svrmod)
        g.render_ids = {i for (t, i) in g.ssf() if t == 'SSFA'}
        return g
    if game.startswith('ps2_'):
        import ps2_port
        if game not in ps2_port.GAMES:
            raise BadInput('game %s is not ready (status research; docs/OTHER_GAMES.md)' % game)
        return ps2_port.GamePS2(game, src.path)
    raise BadInput('game %s is not ready (status research; docs/OTHER_GAMES.md)' % game)


def cmd_list(a):
    S = svr10()
    src = Src(a['src'], os.path.join(a.get('cache') or os.path.join(HERE, 'work', 'src'), a['game']))
    g = open_game(a['game'], src, a.get('svrmod'))
    g11, snd = game11(a['pac11'])
    play = playable11(g11)
    out = []
    for c in g.roster():
        r = g.dat[c]
        woman = g.gender(c, g11) if hasattr(g, 'gender') else r[208]   # (PS2: also finds the 2011 namesake)
        i11 = g.id11(c) if hasattr(g, 'id11') else c
        same = i11 in play and S.norm(g11.name(i11)) == S.norm(g.name(c))
        out.append(dict(key=str(c), name=g.name(c), short=g.short(c), gender='female' if woman else 'male',
                        model=bool(g.has_chpac(c)), render=c in g.render_ids,
                        in_2011=i11 if same else None))
        if hasattr(g, 'pictures'):   # (2K17: renders under hashed names - the user picks one: pictures)
            out[-1]['needs_picture'] = True
    return out


def cmd_pictures(a):
    """pictures --game G --src S --out D [--item K]: a game's unnamed renders (2K17) for a picture picker:
    D/<key>.png thumbnails (128 px, made once; a second call only lists them) -> {pictures: [{key, thumb, w, h,
    name}], guess}; convert --picture <key> takes that picture (the 512 render + its 256 one). guess is null:
    the pictures match no other game's renders (new shoots) and their names are hashed"""
    if not a.get('out'):
        raise BadInput('pictures needs --out')
    outdir = os.path.abspath(a['out'])
    os.makedirs(outdir, exist_ok=True)
    idx = os.path.join(outdir, 'pictures.json')
    if os.path.exists(idx):
        try:
            pics = json.load(open(idx))
            if all(os.path.exists(p['thumb']) for p in pics):
                return dict(pictures=pics, guess=None)
        except (ValueError, KeyError, TypeError):
            pass
    src = Src(a['src'], os.path.join(outdir, '.src_' + a['game']))
    g = open_game(a['game'], src, a.get('svrmod'))
    if not hasattr(g, 'pictures'):
        raise BadInput('%s names its renders by superstar id: no picker needed' % a['game'])
    progress(5, 'reading the pictures')
    out = []
    pics = g.pictures()
    for k, (key, big, _) in enumerate(pics):
        f = os.path.join(outdir, key + '.png')
        im = raw_image(big)
        w, h = im.size
        im.thumbnail((128, 128))
        im.save(f)
        out.append(dict(key=key, thumb=f, w=w, h=h, name=''))
        progress(5 + 95 * (k + 1) // len(pics), 'pictures')
    json.dump(out, open(idx, 'w'))
    shutil.rmtree(os.path.join(outdir, '.src_' + a['game']), ignore_errors=True)
    return dict(pictures=out, guess=None)


def cmd_convert(a):
    if not a.get('item') or not a.get('out'):
        raise BadInput('convert needs --item and --out')
    try:
        cid = int(a['item'])
    except ValueError:
        raise BadInput('--item is a superstar id (from list)')
    outdir = os.path.abspath(a['out'])
    os.makedirs(outdir, exist_ok=True)
    src = Src(a['src'], os.path.join(outdir, '.src_' + a['game']))
    progress(1, 'reading the game')
    g = open_game(a['game'], src, a.get('svrmod'))
    if a.get('picture'):
        if not hasattr(g, 'pick_render'):
            raise BadInput('--picture is for games with unnamed renders (wwe2k17)')
        g.pick_render(cid, a['picture'])
    g11, snd = game11(a['pac11'])
    rep = dict(ok=False, out=None, folder=None, opens='superstar', warnings=[], dropped=[], notes=[])
    o = dict(a, src=src, src_game=g, g11=g11, snd=snd)
    for k in ('base', 'entrance', 'call', 'max_port'):
        if o.get(k) is not None:
            o[k] = int(o[k])
    fn = {'svr2010': build10, 'svr2009': build09, 'svr2008': build09, 'svr2007': build09, 'wwe12': buildW, 'wwe13': buildW,
          'wwe2k14': buildW, 'wwe2k15': buildW, 'wwe2k16': buildW,
          'wwe2k17': buildW}.get(a['game'])
    if fn is None and a['game'].startswith('ps2_'):
        import ps2_port
        fn = ps2_port.build_ps2
    out, mod = fn(cid, outdir, o, rep)
    rep.update(ok=True, out=out, folder=mod)
    progress(100, 'done')
    return rep


def main(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(json.dumps({'error': 'usage: games | list --game G --src S | convert --game G --src S --item K --out D [--picture KEY] | pictures --game G --src S --out D'}))
        return 2
    cmd = argv[1]
    a = dict(pac11=PAC11)
    i = 2
    flags = {'--no-moves': 'no_moves', '--no-port': 'no_port', '--no-taunts': 'no_taunts', '--port-taunts': 'port_taunts',
             '--same-moves': 'same_moves'}
    while i < len(argv):
        k = argv[i]
        if k in flags:
            a[flags[k]] = True
            i += 1
        elif k.startswith('--') and i + 1 < len(argv):
            a[k[2:].replace('-', '_')] = argv[i + 1]
            i += 2
        else:
            print(json.dumps({'error': 'unexpected argument: %s' % k}))
            return 2
    rep_path = None
    try:
        if cmd == 'games':
            res = GAMES
        elif cmd in ('list', 'convert', 'pictures'):
            if a.get('game') not in {g['id'] for g in GAMES}:
                raise BadInput('unknown --game %r (see games)' % a.get('game'))
            if a['game'] not in READY:
                raise BadInput('%s is not ready yet (status research)' % a['game'])
            if not a.get('src'):
                raise BadInput('--src is needed')
            if cmd == 'list':
                res = cmd_list(a)
            elif cmd == 'pictures':
                res = cmd_pictures(a)
            else:
                rep_path = os.path.join(os.path.abspath(a['out']), 'report.json') if a.get('out') else None
                res = cmd_convert(a)
        else:
            raise BadInput('unknown command %r' % cmd)
    except BadInput as e:
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 2
    except Exception as e:   # noqa: BLE001
        traceback.print_exc(file=sys.stderr)
        res = {'ok': False, 'error': '%s: %s' % (type(e).__name__, e), 'opens': 'superstar'}
        if rep_path:
            json.dump(res, open(rep_path, 'w'), indent=1)
        print(json.dumps(res))
        return 3
    if rep_path:
        json.dump(res, open(rep_path, 'w'), indent=1)
    print(json.dumps(res, indent=None if cmd == 'convert' else 1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
