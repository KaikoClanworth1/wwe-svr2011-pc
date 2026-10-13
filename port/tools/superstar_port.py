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

Games:
    svr2010   SvR 2010 - everything (svr10_superstar.py: model, renders, theme, titantron,
              ratings, abilities, the moveset and the moves 2011 lacks as a move pack)
    svr2009   SvR 2009 - model (svr08_char.py), renders, theme, titantron, ratings, entrance,
              announcer; the moveset stays the base's for now (the 2009 slot map is next)
    svr2008   SvR 2008 - the same as 2009 (ids: 2011 id = 2008 id + 100)
    others    listed with status "research" (docs/OTHER_GAMES.md)

--src may be a disc image (read through discfs.py); the files a conversion needs are
copied once to <dir>/.src_<game>/ (pac/ch/chEtc.pac, DLC_HD.pac, the superstar's ch pac,
evt/Nyujyo*.pac, the music bank, the titantron; for 2010 moves also m.pac / misc.pac).
"""
import collections
import contextlib
import io
import json
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
    dict(id='svr2007', name='WWE SmackDown vs. Raw 2007', platform='x360', disc_hint='545107E0', status='research'),
    dict(id='wwe12', name="WWE '12", platform='x360', disc_hint='54510890', status='research'),
    dict(id='wwe13', name="WWE '13", platform='x360', disc_hint='545108B4', status='research'),
    dict(id='wwe2k14', name='WWE 2K14', platform='x360', disc_hint='545408B2', status='research'),
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
        for i, r in pach(ch[(b'CHAR', b'DAT ')]).items():
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


def write_mod(mod, man, out_svrmod):
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
    drop('abilities (%s has none): the base\'s' % g9.label)
    # moves
    if not o.get('no_moves') and base != i11:
        drop('moveset: the base\'s for now (%s -> 2011 slot map not done yet)' % g9.label)
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
    recs = g9.music_records()
    for m in (music, cid):
        if m not in (0, 255) and m * 100 in recs:
            src, gain = theme_from_record(recs[m * 100], os.path.join(mod, 'theme.mp3'), work)
            man['song'] = 'theme.mp3'
            note('theme: music yaf record %d -> theme.mp3 (%.1f LUFS %+.1f dB)' % (m * 100, src, gain))
            break
    else:
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
    txt = write_mod(mod, man, out)
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
    if names and os.path.exists(os.path.join(mod, 'ch.pac')):
        digits = sorted(model_attires(os.path.join(mod, 'ch.pac')))
        man = collections.OrderedDict()
        for line in open(os.path.join(mod, 'manifest.txt'), encoding='utf-8', errors='replace'):
            if '=' in line:
                k, v = line.rstrip('\r\n').split('=', 1)
                man[k] = v
        added = []
        for a in digits:
            if a >= 1 and a in names and 'attire%d_name' % (a + 1) not in man:
                man['attire%d_name' % (a + 1)] = names[a][:31]
                added.append('%d %s' % (a + 1, names[a]))
        if added:
            write_mod(mod, man, out)
            rep['notes'].append('attire names from SvR 2010: ' + ', '.join(added))
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


# --------------------------------------------------------------------------- commands

def open_game(game, src):
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
    if game in ('svr2009', 'svr2008'):
        g = (Game09 if game == 'svr2009' else Game08)(src)
        g.render_ids = {i for (t, i) in g.ssf() if t == 'SSFB'}
        g.dat = g.g.dat
        return g
    raise BadInput('game %s is not ready (status research; docs/OTHER_GAMES.md)' % game)


def cmd_list(a):
    S = svr10()
    src = Src(a['src'], os.path.join(a.get('cache') or os.path.join(HERE, 'work', 'src'), a['game']))
    g = open_game(a['game'], src)
    g11, snd = game11(a['pac11'])
    play = playable11(g11)
    out = []
    for c in g.roster():
        r = g.dat[c]
        i11 = g.id11(c) if hasattr(g, 'id11') else c
        same = i11 in play and S.norm(g11.name(i11)) == S.norm(g.name(c))
        out.append(dict(key=str(c), name=g.name(c), short=g.short(c), gender='female' if r[208] else 'male',
                        model=bool(g.has_chpac(c)), render=c in g.render_ids,
                        in_2011=i11 if same else None))
    return out


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
    g = open_game(a['game'], src)
    g11, snd = game11(a['pac11'])
    rep = dict(ok=False, out=None, folder=None, opens='superstar', warnings=[], dropped=[], notes=[])
    o = dict(a, src=src, src_game=g, g11=g11, snd=snd)
    for k in ('base', 'entrance', 'call'):
        if o.get(k) is not None:
            o[k] = int(o[k])
    fn = {'svr2010': build10, 'svr2009': build09, 'svr2008': build09}[a['game']]
    out, mod = fn(cid, outdir, o, rep)
    rep.update(ok=True, out=out, folder=mod)
    progress(100, 'done')
    return rep


def main(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(json.dumps({'error': 'usage: games | list --game G --src S | convert --game G --src S --item K --out D'}))
        return 2
    cmd = argv[1]
    a = dict(pac11=PAC11)
    i = 2
    flags = {'--no-moves': 'no_moves'}
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
        elif cmd in ('list', 'convert'):
            if a.get('game') not in {g['id'] for g in GAMES}:
                raise BadInput('unknown --game %r (see games)' % a.get('game'))
            if a['game'] not in READY:
                raise BadInput('%s is not ready yet (status research)' % a['game'])
            if not a.get('src'):
                raise BadInput('--src is needed')
            if cmd == 'list':
                res = cmd_list(a)
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
