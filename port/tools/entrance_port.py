"""Entrances from other games -> SvR 2011 entrances: one entry point for the Mod Maker.

  python entrance_port.py games
  python entrance_port.py list --game <id> --src <extract folder or disc image> [--game-pac <2011 pac dir>]
  python entrance_port.py convert --game <id> --src <...> --entrance <n> --number <2011 n> --out <dir>
                                  [--name <text>] [--no-parts] [--game-pac <2011 pac dir>]
  (wwe13 also takes --svrmod <svrmod.exe>: its pacs are xcompressed; default: next to the tools or
   the Mod Maker; missing = exit 2)
  python entrance_port.py free-numbers [--game-pac <2011 pac dir>] [--mods <Mods dir>]

Contract (shared by the Mod Maker's converters, docs/MOD_MAKER_3_PLAN.md):
- stdout carries only the JSON result; logs go to stderr, progress lines on stderr read exactly
  `progress <0-100> <short text>`.
- exit 0 = done, 2 = bad input (unknown game, missing file, bad number), 3 = conversion failed;
  anything else is a crash.
- convert writes <out>/pack/ (a move pack: pack.txt with `pacentry` lines + entrance/*.bin; it
  goes in a superstar mod's moves/ or in Mods/Moves/<name>/, docs/MOVE_PACKS.md) and
  <out>/report.json {ok, game, entrance, number, name, files, warnings[], dropped[], notes[]},
  mirrored to stdout.
- --src is an extract folder or a disc image (tools/discfs.py when present: Xbox 360 XDVDFS /
  PS2 ISO 9660); paths inside it are the disc's own (e.g. pac/evt/Nyujyo0.pac, PAC/EVTENT.PAC).

Games: docs/OTHER_GAMES_ENTRANCES.md (formats and what converts); per-game converters
tools/svr08_entrance.py (SvR 2008), tools/svr10_entrance.py (SvR 2010), tools/hctp_entrance.py (HCTP)
and tools/wwe13_entrance.py (WWE '13).
"""
import collections
import io
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svrfmt  # noqa: E402
import ymks  # noqa: E402
import svr08_entrance as E08  # noqa: E402
import hctp_entrance as EH  # noqa: E402
import svr10_entrance as E10  # noqa: E402
import wwe13_entrance as E13  # noqa: E402


class InputError(Exception):
    """bad input: exit 2"""


def log(*a):
    print(*a, file=sys.stderr)


def progress(pct, text):
    print('progress %d %s' % (pct, text), file=sys.stderr, flush=True)


# ============================================================================ sources

class FolderSource:
    """an extract folder; the same calls as discfs's disc objects"""

    def __init__(self, root):
        self.root = root

    def _p(self, path):
        p = os.path.join(self.root, *path.replace('\\', '/').split('/'))
        if os.path.exists(p):
            return p
        # case-insensitive walk (disc paths are matched without case)
        cur = self.root
        for part in path.replace('\\', '/').split('/'):
            if not part:
                continue
            hit = next((n for n in os.listdir(cur) if n.lower() == part.lower()), None) if os.path.isdir(cur) else None
            if hit is None:
                return None
            cur = os.path.join(cur, hit)
        return cur

    def exists(self, path):
        return self._p(path) is not None

    def listdir(self, path=''):
        p = self._p(path) if path else self.root
        return sorted(os.listdir(p)) if p and os.path.isdir(p) else []

    def read(self, path):
        p = self._p(path)
        if p is None:
            raise InputError('not in the source: %s' % path)
        with open(p, 'rb') as f:
            return f.read()

    def size(self, path):
        return os.path.getsize(self._p(path))


def open_source(path):
    if not path or not os.path.exists(path):
        raise InputError('--src not found: %s' % path)
    try:
        import discfs  # (the Mod Maker's disc reader, D1: folders, X360 XDVDFS, PS2 ISO 9660)
        return discfs.open(path)
    except ImportError:
        if os.path.isdir(path):
            return FolderSource(path)
        raise InputError('reading a disc image needs tools/discfs.py; give an extract folder instead')
    except Exception as x:
        raise InputError('cannot read --src %s: %s' % (path, x))


# ============================================================================ containers

def unbpe(b):
    return svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b


def epk8_read(d):
    """EPK8 (2011's evt/Nyujyo*.pac): TOC at 0x800 - groups {4CC, u16 count*4, u16, u32} each
    followed by 16-byte entries {name[8] space padded, u32 sector, u32 size/0x100} (LE); data from
    0x4000 in 0x800 sectors. -> {(group, name): raw bytes}"""
    out = collections.OrderedDict()
    p = 0x800
    while p + 12 <= 0x4000 and d[p:p + 4].strip(b'\0'):
        tag = d[p:p + 4].decode('latin-1')
        n = struct.unpack_from('<H', d, p + 4)[0] // 4
        q = p + 12
        for i in range(n):
            nm = d[q:q + 8].decode('latin-1').strip()
            sec, sz = struct.unpack_from('<II', d, q + 8)
            o = 0x4000 + sec * 0x800
            out[(tag, nm)] = d[o:o + sz * 0x100]
            q += 16
        p = q
    return out


def pac_entries(d):
    """EPAC or EPK8 -> {(group, name): unpacked bytes}"""
    if d[:4] == b'EPK8':
        return {k: unbpe(v) for k, v in epk8_read(d).items()}
    if d[:4] == b'EPAC':
        # (svrfmt.epac_read's layout, but the count is the u16 at +4: SvR 2007 keeps other data in
        # the u16 at +6)
        out = collections.OrderedDict()
        p = 0x800
        while p < 0x4000 and d[p:p + 4] != bytes(4):
            ty = d[p:p + 4].decode('latin-1')
            cnt = struct.unpack_from('<H', d, p + 4)[0] // 3
            p += 12
            for _ in range(cnt):
                n = d[p:p + 4].replace(bytes(1), b'').decode('latin-1').strip()
                sec, sz = struct.unpack_from('<II', d, p + 4)
                o = 0x4000 + sec * 0x800
                out[(ty, n)] = unbpe(d[o:o + sz * 0x100])
                p += 12
        return out
    raise ValueError('not an EPAC / EPK8 pac')


def evp_name(evp):
    """the 30-byte name of an EVP (big-endian 2008/2011 '0FOP' chunk)"""
    if evp[:4] != b'0FOP':
        return ''
    return evp[0x22:0x40].split(b'\0')[0].decode('latin-1', 'replace').strip()


def norm(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


# ============================================================================ 2011 side

NYUJYO = [(100, 134, '0', '1'), (135, 178, '1', '2'), (179, 271, '2', '3'), (272, 290, '3', '4'),
          (291, 479, '4', '5'), (480, 511, '5', 'E'), (512, 531, '6', 'F'), (532, 551, '7', 'G'),
          (552, 599, '8', 'H'), (600, 999, '9', 'I')]


def find_game_pac(arg):
    """the 2011 pac folder: --game-pac, else the first ancestor of the working directory (or of
    this script) holding pac/evt/Nyujyo5.pac (the Mod Maker runs in <game>/Mods/.convert)"""
    if arg:
        if not os.path.exists(os.path.join(arg, 'evt', 'Nyujyo5.pac')):
            raise InputError('--game-pac has no evt/Nyujyo5.pac: %s' % arg)
        return arg
    for start in (os.getcwd(), HERE):
        p = os.path.abspath(start)
        for _ in range(8):
            for cand in (os.path.join(p, 'pac'), os.path.join(p, 'Game Files', 'pac')):
                if os.path.exists(os.path.join(cand, 'evt', 'Nyujyo5.pac')):
                    return cand
            p = os.path.dirname(p)
    return None


_stock = {}
_stock_evp = {}


def stock_2011_evps(game_pac):
    """{number: EVP bytes} of 2011's own entrances"""
    stock_2011(game_pac)
    return _stock_evp.get(game_pac, {})


def stock_2011(game_pac):
    """{number: name} of 2011's own entrances (main numbers below 10000)"""
    if game_pac in _stock:
        return _stock[game_pac]
    out = {}
    if game_pac:
        for f in sorted(os.listdir(os.path.join(game_pac, 'evt'))):
            if not re.match(r'(?i)nyujyo.\.pac$', f):
                continue
            ents = epk8_read(open(os.path.join(game_pac, 'evt', f), 'rb').read())
            for (g, n), raw in ents.items():
                if g.startswith('EVP') and n.isdigit() and int(n) < 10000:
                    u = unbpe(raw)
                    out[int(n)] = evp_name(u)
                    _stock_evp.setdefault(game_pac, {})[int(n)] = u
    _stock[game_pac] = out
    return out


def pack_numbers(mods):
    """entrance numbers the installed packs already add (pacentry lines)"""
    used = set()
    if mods and os.path.isdir(mods):
        for root, dirs, files in os.walk(mods):
            if 'PacOverlay' in root or os.path.exists(os.path.join(root, 'disabled')):
                continue
            if 'pack.txt' in files:
                for line in open(os.path.join(root, 'pack.txt'), encoding='latin-1'):
                    m = re.match(r'\s*pacentry\s+\S*nyujyo\S*\s+EV[PT]\S\s+(\d+)', line, re.I)
                    if m:
                        n = int(m.group(1))
                        used.add(n if n < 10000 else n % 1000)
    return used


def in_2011(name, game_pac):
    if not name:
        return None
    k = norm(name)
    for n, nm in stock_2011(game_pac).items():
        if norm(nm) == k:
            return n
    return None


# ============================================================================ games

GAMES = collections.OrderedDict([
    ('svr07', dict(name='SmackDown vs. Raw 2007', platform='x360', disc_hint='545107E0', status='ready')),
    ('svr08', dict(name='SmackDown vs. Raw 2008', platform='x360', disc_hint='5451080B', status='ready')),
    ('svr09', dict(name='SmackDown vs. Raw 2009', platform='x360', disc_hint='54510826', status='ready')),
    ('svr10', dict(name='SmackDown vs. Raw 2010', platform='x360', disc_hint='54510844', status='ready')),
    ('wwe12', dict(name="WWE '12", platform='x360', disc_hint='54510890', status='research')),
    ('wwe13', dict(name="WWE '13", platform='x360', disc_hint='545108B4', status='ready')),
    ('wwe2k14', dict(name='WWE 2K14', platform='x360', disc_hint='545408B2', status='research')),
    ('hctp', dict(name='SmackDown! Here Comes the Pain', platform='ps2', disc_hint='SLES_520.36', status='ready')),
])
# (disc_hint: the Xbox 360 title id / PS2 serial discfs.open(iso).info() reports)


# ---- SvR 2008 (Xbox 360): pac/evt/Nyujyo0-3.pac, EPAC groups EVP1-4/EVT1-4, numbers 4000+

def svr08_entrances(src):
    evp, evt = {}, {}
    for f in src.listdir('pac/evt'):
        if not re.match(r'(?i)nyujyo\d\.pac$', f):
            continue
        for (g, n), raw in pac_entries(src.read('pac/evt/' + f)).items():
            if not n.isdigit():
                continue
            (evp if g.startswith('EVP') else evt if g.startswith('EVT') else {})[int(n)] = raw
    return evp, evt


def svr08_list(src, game_pac):
    evp, evt = svr08_entrances(src)
    out = []
    for n in sorted(evp):
        if n >= 10000 or n not in evt:
            continue
        try:
            e = E08.parse_evp(evp[n], '2008')
        except Exception as x:
            log('svr08 %d: EVP not readable (%s)' % (n, x))
            continue
        nm = e['name'].split(b'\0')[0].decode('latin-1', 'replace').strip()
        props = [o['model'] for o in e['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP]
        b = E08.read_evt(evt[n], '2008')
        frames = {}
        for x in b.entries:
            if x['id'] == 0 and x['y'] == 0 and x['x'] in (0, 5, 10, 15, 20):
                frames[x['x'] // 5] = x['frames']
        out.append(dict(number=n, name=nm, segments=len(frames), frames=[frames.get(k, 0) for k in range(5)],
                        objects=len(e['objs']), props=props, has_pyro=any((o['t0'], o['t1']) == E08.TYPE_GLOBAL for o in e['objs']),
                        has_movie=False, has_theme=False, in_2011=in_2011(nm, game_pac)))
    return out


def svr08_convert(src, entrance, number, name, parts, rep):
    evp, evt = svr08_entrances(src)
    if entrance not in evp or entrance not in evt:
        raise InputError('SvR 2008 has no entrance %d' % entrance)
    progress(20, 'converting the script')
    log_lines = []
    e8 = E08.parse_evp(evp[entrance], '2008')
    e11 = E08.convert_evp(e8, number, log=log_lines)
    if name:
        nm = name.encode('latin-1', 'replace')[:29]
        e11['name'] = nm + bytes(30 - len(nm))
    progress(45, 'converting the tracks')
    t11 = E08.convert_evt(evt[entrance], log=log_lines)
    p11 = E08.build_evp(e11)
    rep['notes'] += log_lines
    rep['warnings'] += [l for l in log_lines if l.startswith('WARNING')]
    rep['dropped'] += [l for l in log_lines if l.startswith('dropped')]
    return p11, t11


# ---- HCTP (PS2): PAC/EVTENT.PAC (EVET banks + ENTR/SEVP), names PAC/MOVES.PAC

def hctp_src(src):
    ents = EH.dpac(src.read('PAC/EVTENT.PAC'))
    banks = {int(n): b for g, n, b in ents if g == 'EVET' and n.isdigit()}
    entr = {n: b for g, n, b in ents if g == 'ENTR'}
    names = {}
    if src.exists('PAC/MOVES.PAC'):
        for g, n, b in EH.dpac(src.read('PAC/MOVES.PAC')):
            if g == 'MOVE' and n == 'LIST':
                for i, sub in EH.pac(b):
                    if i == 2:
                        for k in range(struct.unpack_from('<H', sub, 0)[0]):
                            r = sub[2 + 32 * k:2 + 32 * (k + 1)]
                            names[struct.unpack_from('<H', r, 0)[0]] = r[2:32].split(b'\0')[0].decode('latin-1')
    return dict(banks=banks, sevp=entr['SEVP'], sydt=entr.get('SYDT')), names


def hctp_list(src, game_pac):
    s, names = hctp_src(src)
    sevp = EH.parse_sevp(s['sevp'])
    out = []
    for n in sorted(sevp):
        bank = s['banks'].get(n)
        if not bank or len(bank) <= 512:
            continue                               # (placeholders)
        T, ents = EH.H.read_bank(bank)
        kinds = [EH.kind_of(o) for o in sevp[n]['objs']]
        frames = {}
        for o, k in zip(sevp[n]['objs'], kinds):
            if k == 'char':
                for i, sl in enumerate(o['slots'][:5]):
                    e = next((x for x in ents if (x['id'], x['x'], x['y']) == (sl['tid'], sl['tx'], sl['ty'])), None)
                    frames[i] = e['frames'] if e else 0
                break
        out.append(dict(number=n, name=names.get(n, ''), segments=len(frames), frames=[frames.get(k, 0) for k in range(5)],
                        objects=len(kinds), props=[o['model'] for o, k in zip(sevp[n]['objs'], kinds) if k == 'prop'],
                        has_pyro=False, has_movie=False, has_theme=False,
                        in_2011=None))        # (HCTP's are its own motions; 2011 never shipped them)
    return out


def hctp_convert(src, entrance, number, name, parts, rep):
    s, names = hctp_src(src)
    progress(20, 'converting the script and tracks')
    evp, evt, r, notes = EH.build(entrance, number, s, name or names.get(entrance, 'HCTP %d' % entrance))
    rep['notes'] += notes + ['%s: %d' % kv for kv in sorted(r.items())]
    rep['dropped'] += ['%s: %d' % kv for kv in sorted(r.items()) if 'dropped' in kv[0]]
    return evp, evt


# ---- SvR 2010 (Xbox 360): pac/evt/Nyujyo0-3.pac, EPK8 groups EVP1-4/EVT1-4, numbers 100-840

def svr10_entrances(src):
    evp, evt = {}, {}
    for f in src.listdir('pac/evt'):
        if not re.match(r'(?i)nyujyo\d\.pac$', f):
            continue
        for (g, n), raw in pac_entries(src.read('pac/evt/' + f)).items():
            if n.isdigit() and int(n) < 10000:
                (evp if g.startswith('EVP') else evt if g.startswith('EVT') else {})[int(n)] = raw
    return evp, evt


def svr10_list(src, game_pac):
    evp, evt = svr10_entrances(src)
    out = []
    for n in sorted(evp):
        if n not in evt:
            continue
        try:
            e = E08.parse_evp(evp[n], '2011')
        except Exception as x:
            log('svr10 %d: EVP not readable (%s)' % (n, x))
            continue
        nm = evp_name(evp[n])
        frames = {}
        try:
            for x in ymks.read_bank(evt[n]).entries:
                if x['id'] == 0 and x['y'] == 0 and x['x'] in (0, 5, 10, 15, 20):
                    frames[x['x'] // 5] = x['frames']
        except Exception:
            pass
        out.append(dict(number=n, name=nm, segments=len(frames), frames=[frames.get(k, 0) for k in range(5)],
                        objects=len(e['objs']), props=[o['model'] for o in e['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP],
                        has_pyro=any((o['t0'], o['t1']) == E08.TYPE_GLOBAL for o in e['objs']),
                        has_movie=False, has_theme=False, in_2011=in_2011(nm, game_pac)))
    return out


def svr10_convert(src, entrance, number, name, parts, rep, game_pac=None):
    evp, evt = svr10_entrances(src)
    if entrance not in evp or entrance not in evt:
        raise InputError('SvR 2010 has no entrance %d' % entrance)
    progress(20, 'converting the script')
    stock = stock_2011_evps(game_pac)
    if not stock:
        rep['warnings'].append('2011 pac folder not found: object 6 position templates and object 0 model rule skipped')
    m0 = set()
    for b in stock.values():
        try:
            m0.add(E08.parse_evp(b, '2011')['objs'][0]['model'])
        except Exception:
            pass
    lines = []
    e11 = E10.convert_evp(E08.parse_evp(evp[entrance], '2011'), number, m0, E10.obj6_templates(stock), lines)
    if name:
        nm = name.encode('latin-1', 'replace')[:29]
        e11['name'] = nm + bytes(30 - len(nm))
    progress(45, 'converting the tracks')
    t11 = E10.convert_evt(evt[entrance], lines)
    missing = [o['model'] for o in e11['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP and o['model'] in (2403, 2405)]
    if missing:
        rep['warnings'].append('props %s are not in 2011 (they will not show)' % missing)
    rep['notes'] += lines
    rep['dropped'] += [l for l in lines if l.startswith('dropped')]
    return E08.build_evp(e11), t11


# ---- WWE '13 (Xbox 360): pac/evt/Entrance00-08.pac + Entrance_dlc01.pac, xcompressed (svrmod xdec),
#      EPK8 groups 'EVP ' / 'EVZ ' (YANM motion records), numbers 50-856; tools/wwe13_entrance.py

SVRMOD = None          # --svrmod (else found next to the tools / the Mod Maker)
_w13 = {}


def wwe13_entrances(src):
    key = id(src)
    if key not in _w13:
        try:
            sv = E13.find_svrmod(SVRMOD)
        except E13.ToolMissing as x:
            raise InputError(str(x))
        try:
            _w13[key] = E13.read_source(src, sv)
        except FileNotFoundError as x:
            raise InputError(str(x))
    return _w13[key]


def wwe13_list(src, game_pac):
    evp, evz = wwe13_entrances(src)
    return E13.entry_list(evp, evz, game_pac)


def wwe13_convert(src, entrance, number, name, parts, rep, game_pac=None):
    evp, evz = wwe13_entrances(src)
    if entrance not in evp or entrance not in evz:
        raise InputError("WWE '13 has no entrance %d" % entrance)
    if not game_pac:
        rep['warnings'].append('2011 pac folder not found: prop tracks, the lights object and the model checks are skipped')
    progress(15, 'converting the script and tracks')

    def prog(done, total, kind):
        progress(15 + int(40 * done / total), 'tracks %d/%d (%s)' % (done, total, kind))
    evp11, evt11, r = E13.convert(evp[entrance], evz[entrance], number, game_pac, name, progress=prog)
    for k in ('notes', 'warnings', 'dropped'):
        rep[k] += r[k]
    rep['notes'].append('not converted: pyro (prtcls.pac PCE6), titantron (Bink 2), theme, face / finger tracks')
    return evp11, evt11


# ---- SvR 2007 (Xbox 360): pac/evt/evt1.pac + evt2.pac (EPAC, EVP1/EVT1, EVP2/EVT2), 4000-4362
#      (evt0 = the entrance editor and props, evt3 CAE parts, evt4-8 season / win scenes)
# ---- SvR 2009 (Xbox 360): pac/evt/Nyujyo0-3.pac (EPAC, EVP1-4/EVT1-4), 4002-4424 (TNyujyo = tests)
# Both use 2008's script layout (28-byte objects) and its headerless track bank.

def old_entrances(src, files):
    evp, evt = {}, {}
    for f in src.listdir('pac/evt'):
        if not any(re.match(rx, f, re.I) for rx in files):
            continue
        for (g, n), raw in pac_entries(src.read('pac/evt/' + f)).items():
            if n.isdigit() and int(n) < 9999:
                (evp if g.startswith('EVP') else evt if g.startswith('EVT') else {})[int(n)] = raw
    return evp, evt


SRC07 = (r'evt[12]\.pac$',)
SRC09 = (r'nyujyo\d\.pac$',)


def old_list(files):
    def lister(src, game_pac):
        evp, evt = old_entrances(src, files)
        out = []
        for n in sorted(evp):
            if n not in evt:
                continue
            try:
                e = E08.parse_evp(evp[n], '2008')
            except Exception as x:
                log('%d: EVP not readable (%s)' % (n, x))
                continue
            nm = evp_name(evp[n])
            frames = {}
            try:
                for x in E08.read_evt(evt[n], '2008').entries:
                    if x['id'] == 0 and x['y'] == 0 and x['x'] in (0, 5, 10, 15, 20):
                        frames[x['x'] // 5] = x['frames']
            except Exception:
                pass
            out.append(dict(number=n, name=nm, segments=len(frames), frames=[frames.get(k, 0) for k in range(5)],
                            objects=len(e['objs']),
                            props=[o['model'] for o in e['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP],
                            has_pyro=any((o['t0'], o['t1']) == E08.TYPE_GLOBAL for o in e['objs']),
                            has_movie=False, has_theme=False, in_2011=in_2011(nm, game_pac)))
        return out
    return lister


def drop_ops(evt11, op, ys, rep, label):
    """remove every op `op` event from the tracks whose y is in ys (2011 bank bytes in and out)"""
    b = ymks.read_bank(evt11)
    gone = 0
    for e in b.entries:
        if not e['data'] or e.get('share') or e['y'] not in ys:
            continue
        st = ymks.parse_stream(e['data'])
        items, changed = [], False
        for it in st.items:
            if it[0] == 'rec':
                ev, pose = ymks.split_record(it[1])
                if ev:
                    lst, ok = ymks.parse_events(ev, b.T)
                    if ok and any(o == op for o, _ in lst):
                        keep = b''.join(bb for o, bb in lst if o != op)
                        gone += sum(1 for o, _ in lst if o == op)
                        rec = ymks.join_record(keep, pose)
                        items.append(('rec', rec, len(rec) > 0x9f))
                        changed = True
                        continue
            items.append(it)
        if changed:
            e['data'] = ymks.build_stream(ymks.Stream(items))
    for i, e in enumerate(b.entries):
        if e.get('share') and i:
            e['data'] = b.entries[i - 1]['data']
    if gone:
        rep['dropped'].append('%s: %d op-%d events removed' % (label, gone, op))
    return ymks.write_bank(b)


def set_name(e11, name):
    if name:
        nm = name.encode('latin-1', 'replace')[:29]
        e11['name'] = nm + bytes(30 - len(nm))


def svr07_convert(src, entrance, number, name, parts, rep):
    evp, evt = old_entrances(src, SRC07)
    if entrance not in evp or entrance not in evt:
        raise InputError('SvR 2007 has no entrance %d' % entrance)
    progress(20, 'converting the script')
    lines = []
    e7 = E08.parse_evp(evp[entrance], '2008')
    for i, o in enumerate(e7['objs']):      # (Yuke's own 2007 -> 2011 pair 4252 -> 772: extras are 13902)
        if i and (o['t0'], o['t1']) == E08.TYPE_CHAR and o['model'] != E08.GENERIC_CHAR_08:
            lines.append('object %d: extra character model %d -> generic' % (i, o['model']))
            o['model'] = E08.GENERIC_CHAR_08
    e11 = E08.convert_evp(e7, number, log=lines)
    if e11['tail'] and any(r[0] == 9 for r in e11['tail']['recs']):
        lines.append('tail: the 2007-only type-9 record (a music cue) dropped')
        e11['tail']['recs'] = [r for r in e11['tail']['recs'] if r[0] != 9]
    set_name(e11, name)
    progress(45, 'converting the tracks')
    t11 = E08.convert_evt(evt[entrance], log=lines)
    t11 = drop_ops(t11, 79, (1, 2, 3), rep, 'face/hand tracks (2007 loop marker)')
    missing = [o['model'] for o in e11['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP
               and o['model'] in (2040, 2054, 2075, 2078, 2079, 2080, 3001)]
    if missing:
        rep['warnings'].append('props %s are not in 2011 (they will not show)' % missing)
    rep['notes'] += lines
    rep['dropped'] += [l for l in lines if l.startswith('dropped')]
    return E08.build_evp(e11), t11


def svr09_convert(src, entrance, number, name, parts, rep, game_pac=None):
    evp, evt = old_entrances(src, SRC09)
    if entrance not in evp or entrance not in evt:
        raise InputError('SvR 2009 has no entrance %d' % entrance)
    progress(20, 'converting the script')
    stock = stock_2011_evps(game_pac)
    if not stock:
        rep['warnings'].append('2011 pac folder not found: object 6 position templates and object 0 model rule skipped')
    m0 = set()
    for b in stock.values():
        try:
            m0.add(E08.parse_evp(b, '2011')['objs'][0]['model'])
        except Exception:
            pass
    lines = []
    e9 = E08.parse_evp(evp[entrance], '2008')        # (2008's object entries, 2011's model ids)
    e11 = E10.convert_evp(e9, number, m0, E10.obj6_templates(stock), lines)
    if e11['objs'] and e11['objs'][0]['model'] == E08.PREVIEW_CHAR_11:
        for o in e11['objs'][1:7]:                    # (Yuke's: extras follow the preview body)
            if (o['t0'], o['t1']) == E08.TYPE_CHAR and o['model'] == E08.GENERIC_CHAR_11:
                o['model'] = E08.PREVIEW_CHAR_11
    set_name(e11, name)
    progress(45, 'converting the tracks')
    t11 = E08.convert_evt(evt[entrance], log=lines)
    missing = [o['model'] for o in e11['objs'] if (o['t0'], o['t1']) == E08.TYPE_PROP and o['model'] in (2116, 2152, 2262)]
    if missing:
        rep['warnings'].append('props %s are not in 2011 (they will not show)' % missing)
    rep['notes'] += lines
    rep['dropped'] += [l for l in lines if l.startswith('dropped')]
    return E08.build_evp(e11), t11


LISTERS = {'svr07': old_list(SRC07), 'svr09': old_list(SRC09), 'svr08': svr08_list, 'svr10': svr10_list, 'hctp': hctp_list, 'wwe13': wwe13_list}
CONVERTERS = {'svr07': svr07_convert, 'svr09': svr09_convert, 'svr08': svr08_convert, 'svr10': svr10_convert, 'hctp': hctp_convert, 'wwe13': wwe13_convert}


# ============================================================================ commands

def opt(a, k, d=None):
    return a[a.index(k) + 1] if k in a and a.index(k) + 1 < len(a) else d


def cmd_games(a):
    return [dict(id=k, name=v['name'], platform=v['platform'], disc_hint=v['disc_hint'], status=v['status'])
            for k, v in GAMES.items()]


def game_of(a):
    g = opt(a, '--game')
    if g not in GAMES:
        raise InputError('unknown --game %r (games: %s)' % (g, ', '.join(GAMES)))
    if g not in LISTERS:
        raise InputError('%s entrances are not converted yet (research): docs/OTHER_GAMES_ENTRANCES.md' % GAMES[g]['name'])
    return g


def cmd_list(a):
    g = game_of(a)
    src = open_source(opt(a, '--src'))
    progress(5, 'reading entrances')
    out = LISTERS[g](src, find_game_pac(opt(a, '--game-pac')))
    progress(100, '%d entrances' % len(out))
    return out


def free_numbers(game_pac, mods):
    used = set(stock_2011(game_pac)) | pack_numbers(mods)
    return [n for lo, hi, f, gl in NYUJYO for n in range(lo, hi + 1) if n not in used]


def cmd_free(a):
    gp = find_game_pac(opt(a, '--game-pac'))
    if not gp:
        raise InputError('the 2011 pac folder was not found: give --game-pac')
    mods = opt(a, '--mods') or os.path.join(os.path.dirname(gp), 'Mods')
    return free_numbers(gp, mods)


def cmd_convert(a):
    g = game_of(a)
    src = open_source(opt(a, '--src'))
    try:
        entrance, number = int(opt(a, '--entrance')), int(opt(a, '--number'))
    except (TypeError, ValueError):
        raise InputError('--entrance and --number must be numbers')
    out = opt(a, '--out')
    if not out:
        raise InputError('--out is needed')
    if not 100 <= number < 1000:
        raise InputError('--number must be 100..999 (Create-An-Entrance parts are 10000 + 1000k + number)')
    gp = find_game_pac(opt(a, '--game-pac'))
    if gp and number in stock_2011(gp):
        raise InputError('2011 already has entrance %d (%s): pick a free number (free-numbers)' % (number, stock_2011(gp)[number]))
    rep = dict(ok=False, game=g, entrance=entrance, number=number, name=opt(a, '--name', ''), files=[],
               warnings=[], dropped=[], notes=[])
    try:
        progress(5, 'reading the source')
        conv = CONVERTERS[g]
        kw = {'game_pac': gp} if 'game_pac' in conv.__code__.co_varnames else {}
        evp, evt = conv(src, entrance, number, opt(a, '--name'), '--no-parts' not in a, rep, **kw)
        progress(60, 'checking')
        E08.parse_evp(evp, '2011')
        b = ymks.read_bank(evt)
        for e in b.entries:
            if e['data']:
                for f, r in ymks.parse_stream(e['data']).records():
                    ev, _ = ymks.split_record(r)
                    if ev and not ymks.parse_events(ev, b.T)[1]:
                        raise RuntimeError('event block does not walk 2011\'s size table in (%d,%d,%d)' % (e['id'], e['x'], e['y']))
        files = [(number, evp, evt)]
        if '--no-parts' not in a:
            progress(75, 'Create-An-Entrance parts')
            try:
                files += list(E08.cae_parts(evp, evt, number))
            except Exception as x:   # (scripts without the usual 21 slots: tag / special entrances)
                rep['warnings'].append('no Create-An-Entrance parts for this entrance (%s)' % x)
        progress(90, 'writing the pack')
        os.makedirs(out, exist_ok=True)
        label = '%s entrance %d as 2011 %d' % (GAMES[g]['name'], entrance, number)
        d = EH.write_pack(out, number, files, label)
        rep['files'] = sorted(os.path.relpath(os.path.join(r, f), out) for r, _, fs in os.walk(d) for f in fs)
        rep['ok'] = True
    except InputError:
        raise
    except Exception as x:
        rep['warnings'].append('conversion failed: %s' % x)
        os.makedirs(out, exist_ok=True)
        json.dump(rep, open(os.path.join(out, 'report.json'), 'w'), indent=1)
        print(json.dumps(rep))
        sys.exit(3)
    json.dump(rep, open(os.path.join(out, 'report.json'), 'w'), indent=1)
    progress(100, 'done')
    return rep


def main(argv):
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__, file=sys.stderr)
        return 2
    global SVRMOD
    SVRMOD = opt(argv, '--svrmod')
    cmd = {'games': cmd_games, 'list': cmd_list, 'convert': cmd_convert, 'free-numbers': cmd_free}.get(argv[0])
    if not cmd:
        print('unknown command %r' % argv[0], file=sys.stderr)
        return 2
    try:
        res = cmd(argv[1:])
    except InputError as x:
        print(json.dumps(dict(ok=False, error=str(x))))
        log('error: %s' % x)
        return 2
    print(json.dumps(res))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
