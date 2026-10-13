"""PS2 SmackDown superstars -> SvR 2011 superstar mods (superstar_port.py's ps2_* games).

The model, attires, entrance gear and select pictures come from tools/ps2_char.py (HCTP Backstage
Porting's reader for every Yuke's DPAC PS2 disc); this module adds what a superstar mod needs around
them: the game's own names, a 2011 base, an entrance, renders, the manifest.

Games (disc serials as ps2_char.SERIALS):
    ps2_hctp   SmackDown! Here Comes the Pain - names: MOVES.PAC group LIST (a PAC at 0x4000) entry 6, u16
               count then 32-byte records {u16 id, name[30]}; the id is the character's model-id digits
               (XX of XXYZ) read as decimal (model 0x3900 -> 39 John Cena)
    (SYM, SvR 2004 / 2006, the PS2 2007-2010: ps2_char reads their models; names not mapped yet)
Characters ps2_char finds without a name (staff, props, copies) are left out of the list.
Not yet: themes (BGM\\*.AFS, ADX), ratings, movesets - the base's.
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

GAMES = {   # game id -> (ps2_char game name, roster reader)
    'ps2_hctp': 'WWE SmackDown! Here Comes the Pain',
}
SERIALS = {'ps2_hctp': ('SLES_520.36', 'SLUS_207.87')}
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
        self.label = GAMES[game].replace('WWE SmackDown! ', '')
        self.chars = self.g.characters()   # {model char id: {attire: model id}}
        self.names = self._names()
        self.dat = {c: bytes(209) for c in self.roster()}   # (cmd_list reads a gender byte; see gender())
        self.render_ids = {c for c in self.roster() if self.g.portraits(c)} if hasattr(self.g, 'portraits') else set()

    def _read(self, rel):
        d = self.g.disc
        return d.read(rel)

    def _names(self):
        P = self.P
        raw = self._read('PAC/MOVES.PAC')
        lst = P.pac(raw[0x4000:])
        b = lst.get(6, b'')
        out = {}
        if len(b) >= 2:
            n = struct.unpack_from('<H', b, 0)[0]
            for i in range(n):
                o = 2 + 32 * i
                if o + 32 > len(b):
                    break
                nm = b[o + 2:o + 32].split(b'\0')[0].decode('latin-1').strip()
                if nm:
                    out[struct.unpack_from('<H', b, o)[0]] = nm
        return out

    def number(self, c):
        """model char id (0x39) -> the record number (39)"""
        try:
            return int('%x' % c)
        except ValueError:
            return None

    def roster(self):
        return [c for c in sorted(self.chars) if self.number(c) in self.names]

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
    # ratings, abilities, moves: not mapped yet
    drop('ratings, abilities and moves: the base\'s (%s records not mapped yet)' % g.label)
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
    drop('theme and titantron: the base\'s (PS2 BGM *.AFS not mapped yet)')
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
