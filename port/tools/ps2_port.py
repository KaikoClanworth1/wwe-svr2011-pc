"""PS2 SmackDown superstars -> SvR 2011 superstar mods (superstar_port.py's ps2_* games).

The model, attires, entrance gear and select pictures come from tools/ps2_char.py (HCTP Backstage
Porting's reader for every Yuke's DPAC PS2 disc); this module adds what a superstar mod needs around
them: the game's own names, a 2011 base, an entrance, renders, the manifest.

Games (disc serials as ps2_char.SERIALS):
    ps2_sym, ps2_hctp, ps2_svr04, ps2_svr06, ps2_svr07, ps2_svr08, ps2_svr09, ps2_svr10
Names: the game's CHAR records (tools/ps2_records.py, ModMaker's); a model's record number is its character
digits (XX of model id XXYZ) read as decimal (HCTP model 0x3900 -> record 39 John Cena), PS2 2009 the id as a
number, PS2 2010 the digits + 100 (NUMBERING). (HCTP also lists the roster in MOVES.PAC group LIST entry 6:
u16 count, 32-byte {u16 id, name[30]}.)
Characters ps2_char finds without a name (staff, props, copies) are left out of the list.
Ratings / abilities: ratings_port / abilities_port (ps2_records); movesets: ps2_moves (by move name, no motions).
Not yet: themes (BGM\\*.AFS, ADX) - the base's.
"""
import collections
import json
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

GAMES = {   # game id -> name
    'ps2_jbi': 'WWF SmackDown! Just Bring It', 'ps2_sym': 'WWE SmackDown! Shut Your Mouth', 'ps2_hctp': 'WWE SmackDown! Here Comes the Pain',
    'ps2_svr04': 'WWE SmackDown! vs. Raw (PS2)', 'ps2_svr06': 'WWE SmackDown! vs. Raw 2006 (PS2)',
    'ps2_svr07': 'WWE SmackDown vs. Raw 2007 (PS2)', 'ps2_svr08': 'WWE SmackDown vs. Raw 2008 (PS2)',
    'ps2_svr09': 'WWE SmackDown vs. Raw 2009 (PS2)', 'ps2_svr10': 'WWE SmackDown vs. Raw 2010 (PS2)',
}
SERIALS = {'ps2_jbi': ('SLES_504.77', 'SLUS_202.69'), 'ps2_sym': ('SLES_512.83', 'SLUS_205.12'), 'ps2_hctp': ('SLES_520.36', 'SLUS_207.87'),
           'ps2_svr04': ('SLES_527.81', 'SLUS_209.36'), 'ps2_svr06': ('SLES_536.76', 'SLUS_211.34'),
           'ps2_svr07': ('SLES_544.89', 'SLUS_214.27'), 'ps2_svr08': ('SLES_548.79',), 'ps2_svr09': ('SLES_552.51',),
           'ps2_svr10': ('SLES_555.45',)}
# model character id (XX of model id XXYZ) -> CHAR record number: the hex digits read as decimal (+ offset), or
# the id as a number (PS2 2009); PS2 2007-2010 records are the X360 games' (2010's numbered from 100)
UNGROUPED = {'ps2_jbi'}   # (every model its own id, attire 0: a superstar's models share a name)
NUMBERING = {'ps2_svr09': ('int', 0), 'ps2_svr10': ('dec', 100)}
JUNK = re.compile(r'^(edit ?\d+|dlc ?\d+|0|dummy|motion test.*|moves? (set|man).*|.* bra \d+|average|speedy|wbm|green|red|'
                  r'ojl|ktm|omp|ms|unknown d+.*|)$', re.I)   # (SYM's OJL / KTM / OMP / MS: no person)
# (no gender in the name records: the women of the rosters by name - 2011's own gender wins when it has her)
WOMEN = {'lita', 'trish stratus', 'stacy keibler', 'torrie wilson', 'victoria', 'sable', 'stephanie mcmahon', 'jazz',
         'molly holly', 'ivory', 'linda mcmahon', 'dawn marie', 'nidia', 'gail kim', 'jacqueline', 'terri'}


# PS2 name -> 2011 name where they differ (None: a different person under the same name)
ALIASES = {'Steve Austin': 'STONE COLD', 'Vince McMahon': 'MR. MCMAHON', 'The Undertaker': 'UNDERTAKER',
           'Ted DiBiase': None}


def norm(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


class GamePS2:
    """superstar_port's game interface (roster, name, short, has_chpac, render_ids, gender) for a PS2 disc"""

    def __init__(self, game, path):
        import ps2_char as P
        self.P, self.game_id, self.path = P, game, path
        self.g = P.open_game(path)
        serial = (self.g.disc.info() or {}).get('serial')
        if serial and serial not in SERIALS[game]:
            raise ValueError('this disc (%s) is not %s' % (serial, GAMES[game]))
        self.label = GAMES[game].replace('WWE SmackDown! ', '').replace('WWE SmackDown ', '').replace('WWF SmackDown! ', '')
        self.chars = self.g.characters()   # {model char id: {attire: model id}}
        self.names = self._names()
        self.dat = {c: bytes(209) for c in self.roster()}   # (cmd_list reads a gender byte; see gender())
        self.render_ids = set()
        for c in self.roster():
            try:
                if self.g.portraits(c):
                    self.render_ids.add(c)
            except Exception:   # noqa: BLE001  (ps2_char reads pictures by the id's digits: not every game's ids)
                pass

    def _read(self, rel):
        d = self.g.disc
        return d.read(rel)

    def _names(self):
        """{record number: name} from the game's CHAR records (tools/ps2_records.py), junk records left out"""
        import ps2_records as R
        out = {}
        for n, rec in R.table(self).items():
            nm = R.record_name(self.game_id, rec)
            if nm and not JUNK.match(nm):
                out[n] = nm
        return out

    def number(self, c):
        """model char id -> the CHAR record number (HCTP 0x39 -> 39; PS2 2009 0x66 -> 102; PS2 2010 0x10 -> 110)"""
        kind, off = NUMBERING.get(self.game_id, ('dec', 0))
        if kind == 'int':
            return c + off
        try:
            return int('%x' % c) + off
        except ValueError:
            return None

    record_number = number

    def roster(self):
        out = [c for c in sorted(self.chars) if self.number(c) in self.names]
        if self.game_id in UNGROUPED:   # (one model per id: each name once, its first model - same_name() has the rest)
            seen, first = set(), []
            for c in out:
                if self.name(c).lower() not in seen:
                    seen.add(self.name(c).lower())
                    first.append(c)
            out = first
        return out

    def same_name(self, c):
        """the other models of this superstar in a game that keeps one model per id (JBI)"""
        n = self.name(c).lower()
        return [x for x in sorted(self.chars) if x != c and self.number(x) in self.names and self.name(x).lower() == n]

    def name(self, c):
        return self.names.get(self.number(c), '')

    def short(self, c):
        return self.name(c)

    def named(self, c):
        return bool(self.name(c))

    def has_chpac(self, c):
        return c in self.chars

    def match11(self, c, g11):
        """2011's playable superstar of the same name, or None (remembered for id11)"""
        import superstar_port as SP
        if not hasattr(self, '_by11'):
            S = SP.svr10()
            self._by11 = {}
            for i in SP.playable11(g11):
                self._by11.setdefault(norm(g11.name(i)), i)
            for k, v in ALIASES.items():
                self._by11[norm(k)] = self._by11.get(norm(v)) if v else None
        self._m11 = getattr(self, '_m11', {})
        self._m11[c] = self._by11.get(norm(self.name(c)))
        return self._m11[c]

    def id11(self, c):
        return getattr(self, '_m11', {}).get(c) or -1

    def gender(self, c, g11=None):
        """1 = woman: 2011's record of the same name, else the roster table"""
        i = self.match11(c, g11) if g11 is not None else None
        if i is not None:
            return 1 if g11.dat[i][208] else 0
        return 1 if self.name(c).lower() in WOMEN else 0


def build_ps2(cid, outdir, o, rep):
    import superstar_port as SP
    S = SP.svr10()
    g, g11, snd, pac11 = o['src_game'], o['g11'], o['snd'], o['pac11']
    if cid not in g.chars or not g.named(cid):
        raise SP.BadInput('%s has no superstar %x' % (g.label, cid))
    name = g.name(cid)
    mid = re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_') + '_' + g.game_id.replace('ps2_', '')
    work = os.path.join(outdir, '.work_%x' % cid)
    mod = os.path.join(outdir, mid)
    for d in (work, mod):
        if os.path.exists(d):
            shutil.rmtree(d)
        os.makedirs(d)
    note, warn, drop = rep['notes'].append, rep['warnings'].append, rep['dropped'].append
    SP.progress(5, 'base superstar')
    woman = g.gender(cid, g11)
    same = [g.match11(cid, g11)] if g.match11(cid, g11) is not None else []
    if o.get('base'):
        base, why = o['base'], 'option'
    elif same:
        base, why = same[0], '2011 has him: his own moves, entrance and commentary'
    else:
        cands = [c for c in S.candidates_11(g11) if g11.dat[c][208] == woman]
        sc = sorted(struct.unpack_from('<H', g11.dat[c], 28)[0] for c in cands)
        med = sc[len(sc) // 2]
        base = min(cands, key=lambda c: abs(struct.unpack_from('<H', g11.dat[c], 28)[0] - med))
        why = 'a %s of middling build (the PS2 records give no class yet)' % ('woman' if woman else 'man')
    note('base %d %s (%s)' % (base, g11.name(base), why))
    man = collections.OrderedDict(type='superstar', id=mid, name=name[:31], short=name[:31], base=base,
                                  author=g.label + ' port', made_with='Port tools (%s)' % g.label, version='1.0')
    # model + renders (ps2_char)
    SP.progress(15, 'model')
    ats = sorted(g.chars[cid])
    pick = [int(x) for x in o['attires'].split(',')] if o.get('attires') else ats[:4]
    if g.game_id in UNGROUPED and g.same_name(cid):
        drop('%d more %s model(s) of his (%s): one per mod for now' % (len(g.same_name(cid)), g.label,
                                                                      ', '.join('%x' % x for x in g.same_name(cid))))
    if len(ats) > 4 and not o.get('attires'):
        drop('attires %s (a mod holds 4; --attires to choose)' % ', '.join(str(a) for a in ats[4:]))
    pw = os.path.join(work, 'ps2_char')
    tmpl = SP.g11_chpac(g11, base) if hasattr(SP, 'g11_chpac') else os.path.join(pac11, 'ch', 'ch%d.pac' % base)
    if not os.path.exists(tmpl):
        tmpl = os.path.join(pac11, 'ch', 'ch104.pac')
    import subprocess
    args = [sys.executable, os.path.join(HERE, 'ps2_char.py'), g.path, '%x' % cid, pw, '--template', tmpl,
            '--attire', ','.join(str(a) for a in pick)]
    r = subprocess.run(args, capture_output=True, text=True)
    try:
        pr = json.load(open(os.path.join(pw, 'report.json')))
    except (OSError, ValueError):
        raise RuntimeError('ps2_char failed: %s' % (r.stderr or r.stdout)[-600:])
    if not pr.get('ok') or not os.path.exists(os.path.join(pw, 'ch.pac')):
        raise RuntimeError('ps2_char: %s' % (pr.get('error') or 'no ch.pac'))
    shutil.copyfile(os.path.join(pw, 'ch.pac'), os.path.join(mod, 'ch.pac'))
    note('model: %s character %x attires %s (ps2_char: %s)' % (g.label, cid, ', '.join(
        '%s %s' % (s['attire'], s['model_id']) for s in pr.get('sources', [])),
        '%d attire(s), %d with entrance gear' % (len(pr.get('sources', [])), sum(1 for s in pr.get('sources', []) if s.get('gear')))))
    for s in pr.get('sources', []):
        if s.get('gear'):
            note('attire %s: entrance gear %s (kind 4)' % (s['attire'], ', '.join(s['gear'])))
    for f in pr.get('fitted', []):   # (ps2_char fits the 192-page budget: what it gave up)
        drop('model: %s' % f)
    for w in pr.get('warnings', [])[:8]:
        warn('model: %s' % w)
    if pr.get('height_m') and not o.get('height'):
        note('height %.2f m (the model\'s own scale: no height= needed)' % pr['height_m'])
    rn = pr.get('renders') or {}
    for f in ('render.dds', 'render_small.dds'):   # (ps2_char writes them from the PS2 select pictures)
        if os.path.exists(os.path.join(pw, f)):
            shutil.copyfile(os.path.join(pw, f), os.path.join(mod, f))
    if rn:
        note('renders: the PS2 select pictures (%s)' % ', '.join('%s from the %s' % kv for kv in sorted(rn.items())))
    else:
        warn("no %s select picture: the base's renders" % g.label)
    # ratings, abilities: the PS2 character records (tools/ratings_port.py, abilities_port.py, ps2_records.py)
    import ratings_port
    rr, rrep = ratings_port.map_ratings(g.game_id, cid, g)
    if rr:
        man['ratings'] = ','.join(map(str, rr))
        note(ratings_port.report_line(g.label, rr, rrep))
    else:
        drop('ratings: the base\'s (%s)' % rrep['note'])
    SP.abilities_step(g.game_id, g, cid, man, note)
    # moves: the PS2 profile's moves that 2011 has (tools/ps2_moves.py; PS2 motions aren't ported)
    import ps2_moves
    SP.progress(55, 'moveset')
    ps2_moves.moveset_step(g, cid, base, o, work, mod, man, rep)
    # entrance
    SP.progress(60, 'entrance')
    if o.get('entrance'):
        man['entrance'] = o['entrance']
    else:
        e, via = SP.entrance_from_names([name], g11)
        be = g11.entrance_block(base)[2]
        if not e and not same:
            e, via = (483 if woman else 522), 'generic (2011 has none of his)'
        if e and e != be:
            man['entrance'] = e
        note('entrance %s "%s" (%s)' % (man.get('entrance', be), g11.evp().get(man.get('entrance', be), '?'),
                                         via or 'the base\'s'))
    # theme + titantron (tools/ps2_media.py, Extras'): the profile's music / movie
    SP.progress(70, 'theme, titantron')
    import ps2_media
    got = ps2_media.media(g.game_id, g.g.disc, g.number(cid), mod, os.path.join(work, 'media'), rep, o.get('vgmstream'))
    if got is None:
        drop('theme and titantron: the base\'s (%s\'s music / movies not read yet)' % g.label)
    else:
        man.update(got)
    # announcer, commentary
    nm, clips = snd.announcer([name])
    if nm:
        man['announcer'] = nm
        note('announcer %s (2011 has his name calls)' % nm)
    if not same:
        man['call'] = o.get('call') if o.get('call') is not None else (69 if woman else 77)
    if o.get('height'):
        man['height'] = '%.2f' % round(min(1.25, max(0.80, float(o['height']))), 2)
    SP.progress(95, 'packing')
    out = os.path.join(outdir, mid + '.svrmod')
    txt = SP.write_mod(mod, man, out, rep, o['pac11'])
    SP.log(txt)
    return out, mod
