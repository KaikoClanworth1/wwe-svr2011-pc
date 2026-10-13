"""Moves from other WWE games as SvR 2011 move packs - the converter the Mod
Maker calls (docs/MOD_MAKER_3_PLAN.md section 4, docs/OTHER_GAMES.md,
docs/MOVE_PACKS.md).

    py moves_port.py games
    py moves_port.py list    --game <id> --src <ISO or extract folder> [--pac11 <2011 pac>]
    py moves_port.py convert --game <id> --src <...> --item <move id> --out <dir> [options]

stdout is only JSON; logs go to stderr, with progress lines `progress <0-100> <text>`.
Exit 0 ok, 2 bad input, 3 failed.

convert writes a move pack <dir>/<pack>/ (pack.txt + motions/, docs/MOVE_PACKS.md; install
as Mods/Moves/<pack> or a superstar mod's moves/ folder) and <dir>/report.json:
    {ok, out, folder, opens: "moves", id, name, template, warnings[], dropped[], notes[]}
The move gets a 2011 move id (`id`): a superstar mod's moves.txt can name it.

Options (convert):
    --template N   the 2011 move whose record, per-key data, events and banks the new move takes
                   (default: the 2011 move of the same category whose length is closest)
    --id N         the 2011 record to use (default: a free one - a record 2011 keeps without
                   motions - with the same name if there is one, else the first free one)
    --name TEXT    the move's name (default: the source game's)

Games:
    wwe12   WWE '12 - motions are WWE '13's ZMOT/YANM curves on the Biped rig: decoded by
            yanm_decode.py and retargeted to 2011's body bones by w13_retarget.py (the
            HCTP Entrance chat's, shared); WWE '12 bodies are plain FK (ik=False).
    wwe13   WWE '13 - the same, in xcompressed pacs (svrmod xdec: --svrmod or found as
            wwe13_entrance.py finds it); its bodies use the IK stream
    wwe2k14 WWE 2K14 - the same; move records in createmode.pac, categories coded (templates from
            every category)
    svr2007 / svr2008 / svr2009  SvR 2007-2009 - their m.pac banks are 2011's YMKs banks without the
            16-byte header (same 0x7d body records, same move ids as 2011): the tracks are copied (Sabu's
            make08 method) - every x / y of the move, 2008-only event ops 192 / 193 dropped, jumps to the
            move pointed at the new id - into the banks of a 2011 template of the same kind (its record,
            EXH / sound / MBD records, sound frames scaled to the new length). Moves on the old YMBs banks
            (submissions, taunts) aren't read yet.

How a move is made (the WWE '13 Mankind moves' method): the attacker (y 0) and the victim
(y 1) of every x the template has are the source's blocks 0 and 1, one key a frame at 30 Hz,
the attacker's start placed where the template's starts (both tracks moved alike: the victim
keeps the source's place relative to the attacker); prop and camera tracks and reversal
branches (x 90-92) are the template's. The template's in-stream events are moved in time
with the new length, its jumps to itself point to the new move. Its record (pack `wazecopy`),
name (`wazename`), per-key EXH / sound / MBD records (exh / evt / mbd) go with it. Not
carried: finger poses (the template's hands), the source's own sounds and damage timing
(the template's, retimed). A template on YMBs banks (a taunt; moveset_port passes the base's) gets the
source's attacker (and victim) as YMBs channels: ymbs_retarget.py solves the game's limb IK + rotations
(worst joint error in the notes), ymbs.py encodes; the motion keeps the template's header times.
"""
import json
import os
import re
import struct
import sys
import traceback
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ch_tool  # noqa: E402
import svrfmt  # noqa: E402

PAC11 = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'Game Files', 'pac')

GAMES = [
    dict(id='wwe12', name="WWE '12", platform='x360', disc_hint='54510890', status='ready'),
    dict(id='wwe13', name="WWE '13", platform='x360', disc_hint='545108B4', status='ready'),
    dict(id='wwe2k14', name='WWE 2K14', platform='x360', disc_hint='545408B2', status='ready'),
    dict(id='svr2010', name='WWE SmackDown vs. Raw 2010', platform='x360', disc_hint='54510844', status='research'),
    dict(id='svr2009', name='WWE SmackDown vs. Raw 2009', platform='x360', disc_hint='54510826', status='ready'),
    dict(id='svr2008', name='WWE SmackDown vs. Raw 2008', platform='x360', disc_hint='5451080B', status='ready'),
    dict(id='svr2007', name='WWE SmackDown vs. Raw 2007', platform='x360', disc_hint='545107E0', status='ready'),
]
OLD = ('svr2007', 'svr2008', 'svr2009')
READY = {g['id'] for g in GAMES if g['status'] == 'ready'}
# WWE '12 ZMVM category -> SvR 2011 MVMT bank (moveset menu categories)
CATS = {'STAT': 'STAT', 'BASE': 'BASE', 'STN1': 'STN1', 'STN2': 'STN2', 'GRND': 'GRND', 'CORN': 'CORN',
        'CETB': 'CETB', 'ROPE': 'ROPE', 'APRO': 'APRO', 'RUN ': 'RUN ', 'TEAM': 'TEAM', 'FINI': 'FINI',
        'SPE1': 'STN1', 'SPE2': 'STN2', 'ANAS': 'ANAS', 'RUMB': 'RUMB', 'LADR': 'LADR', 'CELL': 'CELL',
        'ABIL': 'ABIL', 'DIVI': 'CORN', 'APR2': 'APRO', 'ANA2': 'ANAS', 'WEAP': 'STAT', 'APR1': 'APRO', 'SPE0': 'STN1'}


class BadInput(Exception):
    pass


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def progress(pct, text):
    print('progress %d %s' % (pct, text), file=sys.stderr, flush=True)


def norm(s):
    s = s.lower().replace('_', ' ').strip()
    return re.sub(r'[^a-z0-9]', '', re.sub(r'^\d+ ', '', s))


def read_pac(raw):
    """EPAC / EPK8 groups [(type, [(name, data)])] whichever the entry size: bit 0 of a group's
    flags (u16 after the count) picks 16-byte entries with 8-character names, else 12-byte with 4;
    the magic doesn't decide it (WWE '13's misc.pac says EPK8 with 12-byte entries, 2K14's ch pacs
    EPAC with 16-byte ones); a table that doesn't read is tried the other way"""
    def parse(prefer_wide):
        out, q = [], 0x800
        while q + 12 <= 0x4000:
            typ = raw[q:q + 4]
            if typ == b'\0\0\0\0':
                break
            cnt, flags = struct.unpack_from('<HH', raw, q + 4)
            wide = bool(flags & 1) if prefer_wide is None else prefer_wide
            esz = 16 if wide else 12
            n = cnt // (4 if wide else 3)
            q += 12
            ents = []
            for _ in range(n):
                if q + esz > 0x4000:
                    raise ValueError('table')
                nm = raw[q:q + esz - 8]
                sec, sz = struct.unpack_from('<II', raw, q + esz - 8)
                o = 0x4000 + sec * 0x800
                if o + sz * 0x100 > len(raw) + 0x800:
                    raise ValueError('entry')
                ents.append((nm.split(b'\0')[0].strip(), raw[o:o + sz * 0x100]))
                q += esz
            out.append((typ, ents))
        return out
    for pref in (None, False, True):
        try:
            return parse(pref)
        except (ValueError, struct.error):
            continue
    raise ValueError('not a pac table')


def unz(b):
    """a pac entry: BPE (Yuke's) or ZLIB ("ZLIB", u32, u32 packed, u32 size, zlib data at +16)"""
    if b[:4] == b'ZLIB':
        return zlib.decompress(b[16:])
    return ch_tool.unpack(b)


# --------------------------------------------------------------------------- WWE '12

class Game12:
    """WWE '12: misc.pac MOVS/WAZE (160-byte records from 0x430: name +0x10, id +0x90);
    m.pac ZMVM/<category>: PACH trees ... -> x -> move id -> a '13 motion record (YANM blocks)."""

    def __init__(self, src):
        self.src = src
        g = read_pac(self.pac_bytes('pac/misc.pac'))
        raw = unz(dict(((t, n), b) for t, es in g for n, b in es)[(b'MOVS', b'WAZE')])
        self.waze = waze_records(raw)
        self._m = None
        self._index = None

    def pac_bytes(self, rel):
        return open(self.src.get(rel), 'rb').read()

    def motion_pacs(self):
        return ['pac/m.pac']

    def _groups(self):
        if self._m is None:
            self._m = []
            self.skipped = []
            for rel in self.motion_pacs():
                try:
                    self._m += read_pac(self.pac_bytes(rel))
                except Exception as e:   # noqa: BLE001  (a pac svrmod can't decode: its moves are left out)
                    self.skipped.append('%s (%s)' % (rel, str(e)[:120]))
                    log('moves_port: %s skipped: %s' % (rel, e))
        return self._m

    def index(self):
        """{move id: {x: (category, [path ids])}} over ZMVM"""
        if self._index is None:
            self._index = {}
            for t, es in self._groups():
                if t != b'ZMVM':
                    continue
                for n, b in es:
                    cat = n.decode('latin1')
                    try:
                        top = unz(b)
                    except Exception:   # noqa: BLE001
                        continue

                    def walk(raw, path):
                        if raw[:4] == b'PACH':
                            for i, d in svrfmt.pach_read(raw):
                                try:
                                    walk(unz(d), path + [i])
                                except Exception:   # noqa: BLE001
                                    pass
                        elif len(path) >= 2:
                            self._index.setdefault(path[-1], {}).setdefault(path[-2], (cat, path))
                    walk(top, [])
        return self._index

    def record(self, cat, path):
        b = next(bb for t, es in self._groups() if t == b'ZMVM' for n, bb in es if n == cat.encode('latin1'))
        raw = unz(b)
        for i in path:
            raw = unz(dict(svrfmt.pach_read(raw))[i])
        return raw


class Game13(Game12):
    """WWE '13: the same motions and move records, in xcompressed (0x0FF512ED) pacs - decoded by
    the Mod Maker's svrmod xdec (wwe13_entrance.find_svrmod / xdec); misc.pac MOVS/WAZE (an "EPK8"
    with 12-byte entries); ZMVM in mot06-08.pac (and mot0x / dlc_mot ones that have it)."""

    def __init__(self, src, svrmod=None):
        import wwe13_entrance as WE
        self.WE = WE
        self.svrmod = WE.find_svrmod(svrmod)
        Game12.__init__(self, src)

    def pac_bytes(self, rel):
        raw = open(self.src.get(rel), 'rb').read()
        return self.WE.xdec(raw, self.svrmod) if raw[:4] == b'\x0f\xf5\x12\xed' else raw

    def motion_pacs(self):
        return ['pac/' + n for n in sorted(self.src.d.listdir('pac')) if re.match(r'(dlc_)?mot\d+\.pac$', n.lower())]


class Game14(Game13):
    """WWE 2K14: as WWE '13, its move records in createmode.pac MOVS/WAZE (the name 8 bytes earlier
    in the record: waze_records finds the grid), ZMVM in mot07 / mot08.pac with categories renamed to
    codes (C000, M001, ...): templates are looked for in every category."""

    def __init__(self, src, svrmod=None):
        import wwe13_entrance as WE
        self.WE = WE
        self.svrmod = WE.find_svrmod(svrmod)
        self.src = src
        g = read_pac(self.pac_bytes('pac/createmode.pac'))
        raw = unz(next(b for t, es in g if t == b'MOVS' for n, b in es if n == b'WAZE'))
        self.waze = waze_records(raw)
        self._m = None
        self._index = None


class Game0789:
    """SvR 2007-2009: misc.pac MOVS/WAZE (144-byte records, name +0x10, id +0x88); every m.pac bank whose
    data reads as a YMKs bank once given the 16-byte header (the old YMBs ones don't)."""

    def __init__(self, src, work=None):
        import pickle
        import svr10_moves as SM
        self.src, self.SM = src, SM
        g = read_pac(open(src.get('pac/misc.pac'), 'rb').read())
        raw = unz(dict(((t, n), b) for t, es in g for n, b in es)[(b'MOVS', b'WAZE')])
        self.waze = {}
        for q in range(0, len(raw) - 143, 144):
            nm = raw[q + 0x10:q + 0x50].split(b'\0')[0]
            if len(nm) > 2 and all(32 <= c < 127 for c in nm):
                self.waze.setdefault(struct.unpack_from('<H', raw, q + 0x88)[0], nm.decode('latin1').strip())
        self.groups = SM.read_epac(src.get('pac/m.pac'))[1]
        cache = os.path.join(work, 'index0789.pkl') if work else None
        if cache and os.path.exists(cache):
            self.banks = pickle.load(open(cache, 'rb'))
        else:
            self.banks = {}   # path -> [(id, x, y, frames)]
            for t, es in self.groups:
                for n, b in es:
                    self._walk(SM.unpack(b), [t.decode('latin1').strip(), n.decode('latin1').strip()])
            if cache:
                os.makedirs(work, exist_ok=True)
                pickle.dump(self.banks, open(cache, 'wb'))
        self._index = None
        self.skipped = []

    @staticmethod
    def as_bank(raw):
        import ymks
        if len(raw) < 0x104 or raw[:4] in (b'PACH', b'YMKs', b'YMBs', b'PAC '):
            return None
        n = struct.unpack_from('<I', raw, 0x100)[0]
        if not 0 < n < 20000 or 0x104 + 16 * n > len(raw):
            return None
        try:
            return ymks.read_bank(b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + raw)
        except Exception:   # noqa: BLE001
            return None

    def _walk(self, raw, path):
        if raw[:4] == b'PACH':
            for i, d in svrfmt.pach_read(raw):
                self._walk(self.SM.unpack(d), path + ['%x' % i])
            return
        b = self.as_bank(raw)
        if b is not None:
            self.banks['/'.join(path)] = [(e['id'], e['x'], e['y'], e['frames']) for e in b.entries]

    def bank(self, path):
        parts = path.split('/')
        blob = self.SM.entry_get(self.groups, parts[0].ljust(4).encode(), parts[1].ljust(4).encode())
        raw = self.SM.tree_get(blob, [int(x, 16) for x in parts[2:]])
        return self.as_bank(raw)

    def index(self):
        """{move id: {x: (bank group, path)}} - per move, the bank with the most of its tracks"""
        if self._index is None:
            best = {}
            for path, ents in self.banks.items():
                cnt = {}
                for mid, x, y, fr in ents:
                    cnt[mid] = cnt.get(mid, 0) + 1
                for mid, c in cnt.items():
                    if c > best.get(mid, (0, ''))[0] or (c == best[mid][0] and path.startswith('MOT/')):
                        best[mid] = (c, path)
            self._index = {}
            for mid, (c, path) in best.items():
                for m2, x, y, fr in self.banks[path]:
                    if m2 == mid:
                        self._index.setdefault(mid, {})[x] = (path.split('/')[1], path)
        return self._index


def clean_events08(evs, old, new):
    """2008-only event ops 192 / 193 dropped; jumps (ops 49 / 50) to the move -> its new id"""
    out = []
    for e in evs:
        if e and e[0] in (192, 193):
            continue
        e = bytearray(e)
        if e and e[0] in (49, 50) and len(e) >= 3 and struct.unpack_from('<H', e, 1)[0] == old:
            struct.pack_into('<H', e, 1, new)
        out.append(bytes(e))
    return out


def convert0789(a, rep, outdir, g):
    import ymks
    mid = int(a['item'])
    idx = g.index()
    if mid not in idx:
        raise BadInput("%s has no motion for move %d on a YMKs bank" % (a['game'], mid))
    name = (a.get('name') or g.waze.get(mid) or 'MOVE %d' % mid)[:63]
    path0 = idx[mid][min(idx[mid])][1]
    b8 = g.bank(path0)
    tracks = [e for e in b8.entries if e['id'] == mid]
    e00 = next((e for e in tracks if e['x'] == 0 and e['y'] == 0), tracks[0])
    nf = e00['frames']
    rep['notes'].append('source bank %s: %d tracks, %d frames' % (path0, len(tracks), nf))
    e01 = next((e for e in tracks if e['x'] == 0 and e['y'] == 1), None)
    gap = None
    if e01 is not None:
        try:
            gap = start_gap(ymks.body_keys(e00['data'], b8.T), ymks.body_keys(e01['data'], b8.T))
        except Exception:   # noqa: BLE001
            gap = None
    progress(25, '2011 banks')
    g11 = a.get('_g11') or Game11(a['pac11'], os.path.join(outdir, '.work_cache'))
    tid, how = (int(a['template']), 'option') if a.get('template') else (None, '')
    paired = e01 is not None
    if not tid and mid in g11.waze and any(g11.waze[mid][0]):   # (2011 keeps the move's record - cut
        bits = g11.waze[mid][0]                                           # ones have no category bits)
        same = {i for i, (b, n) in g11.waze.items() if b == bits and i in g11.moved}
        if same:
            tid = choose_template(g11, '', nf, gap, same, paired)
            how = 'the same move category bits, the closest start gap and length'
    if not tid:
        tid = choose_template(g11, '', nf, gap, None, paired)
        how = ('two-person moves: ' if paired else 'one-person moves (a strike): ') + 'the closest start gap and length'
    if not tid or tid not in g11.waze:
        raise BadInput('no 2011 template move; give --template')
    if a.get('id'):
        new_id, why = int(a['id']), 'option'
    elif mid in g11.waze and mid not in g11.moved and mid > 1000:
        new_id, why = mid, 'the same id: a record 2011 keeps without a motion'
    else:
        new_id, why = g11.free_record(name)
    if not new_id:
        raise RuntimeError('no free 2011 move record')
    rep['notes'].append('template %d %s (%s)' % (tid, g11.waze[tid][1], how))
    rep['notes'].append('2011 id %d (%s)' % (new_id, why))
    pack = re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')[:40] or 'move_%d' % mid
    pd = os.path.join(outdir, pack)
    os.makedirs(os.path.join(pd, 'motions'), exist_ok=True)
    lines = ['wazecopy %d %d' % (new_id, tid), 'wazename %d %s' % (new_id, name)]
    progress(50, 'tracks')
    dlen = None
    nmot = 0
    for path, kind, ks in g11.banks_of(tid):
        if kind != 'YMKs':
            rep['dropped'].append('bank %s (%s)' % (path, kind))
            continue
        pac, bank = g11.bank(path)
        if dlen is None:
            d0 = bank.find(tid, 0, 0)
            dlen = d0['frames'] if d0 else None
        packed = path.startswith('MOTP')
        for e in tracks:
            x, y = e['x'], e['y']
            if path.startswith('MVMT') and (x, y) not in ((0, 0), (0, 1)) and not any(xx == x for xx, yy in ks):
                continue   # (menu banks: the body tracks only)
            keys = None
            if y in (0, 1, 2):
                try:
                    keys = ymks.body_keys(e['data'], b8.T, False)
                except Exception:   # noqa: BLE001
                    keys = None
            if keys is not None:
                for k in keys:
                    k['events'] = clean_events08(k['events'], mid, new_id)
                    if not k.get('has_pose', True):
                        k['root'] = k['bones'] = None
                data = ymks.build_motion(keys, max(1, e['frames']), packed=packed and y in (0, 1))
            else:
                if packed:
                    continue   # (MOTP: the characters only)
                data = e['data']
            fn = 'motions/%d_%d_%d_%s.ymk' % (new_id, x, y, path.replace('/', '').lower())
            open(os.path.join(pd, fn), 'wb').write(data)
            lines.append('motion %s %s %d %d %d %d %s' % (pac, path, new_id, x, y, e['frames'], fn))
            nmot += 1
    scale = nf / float(dlen or nf)
    for kind, grp, r, ev in g11.records(tid):
        r = bytearray(r)
        struct.pack_into('<H', r, 0, new_id)
        if kind == 'evt':   # (sound frames to the new length)
            ev2 = bytearray(ev)
            for o in range(0, len(ev2) - 3, 4):
                sid, fr = struct.unpack_from('<Hh', ev2, o)
                struct.pack_into('<Hh', ev2, o, sid, int(round(fr * scale)))
            lines.append('evt %d %s %s' % (grp, bytes(r).hex(), bytes(ev2).hex()))
        else:
            lines.append('%s %d %s' % (kind, grp, bytes(r).hex()))
    open(os.path.join(pd, 'pack.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    rep['notes'].append('%d motions, the source\'s tracks copied (%d frames; template %s)' % (nmot, nf, dlen))
    rep['dropped'].append('2008-only event ops 192 / 193; the source\'s own sounds (the template\'s, retimed)')
    rep.update(ok=True, out=pd, folder=pd, id=new_id, name=name, template=tid)
    progress(100, 'done')
    return rep


def waze_records(raw):
    """{move id: name} from a MOVS/WAZE table: 160-byte records (name +0x10, id +0x90) on a grid whose
    start differs by game ('12 0x430): the start where the most records hold a readable name"""
    def score(s):
        ok = 0
        for k in range(64):
            nm = raw[s + 160 * k + 0x10: s + 160 * k + 0x30].split(b'\0')[0]
            ok += bool(nm) and all(32 <= c < 127 for c in nm)
        return ok
    start = max(range(0x10, 0x1000, 8), key=score)
    out = {}
    for s in range(start, len(raw) - 159, 160):
        r = raw[s:s + 160]
        if not r.strip(b'\0'):
            continue
        nm = r[0x10:0x50].split(b'\0')[0]
        if nm and all(32 <= c < 127 for c in nm):
            out.setdefault(struct.unpack_from('<H', r, 0x90)[0], nm.decode('latin1').strip())
    return out


# --------------------------------------------------------------------------- SvR 2011

class Game11:
    def __init__(self, pac11, work):
        import svr10_moves as SM
        import svr10_superstar as SS
        if not os.path.exists(os.path.join(pac11, 'misc.pac')):
            raise BadInput('--pac11 is not an SvR 2011 pac folder: %s' % pac11)
        self.pac11, self.SM = pac11, SM
        self.waze = SS.Game(pac11, 11).waze()          # {id: (bits, name)}
        os.makedirs(work, exist_ok=True)
        self.idx = SM.bank_index(pac11, os.path.join(work, 'bank_index11.pkl'))
        self.moved = set()
        for path, (kind, keys, size) in self.idx.items():
            for k in keys:
                self.moved.add(k[0])
        self._g = {}

    def groups(self, pac):
        if pac not in self._g:
            self._g[pac] = self.SM.read_epac(os.path.join(self.pac11, pac))[1]
        return self._g[pac]

    def bank(self, path):
        import ymks
        pac = 'mpsp.pac' if path.startswith('MOTP') else 'm.pac'
        parts = path.split('/')
        blob = self.SM.entry_get(self.groups(pac), parts[0].ljust(4).encode(), parts[1].ljust(4).encode())
        if len(parts) > 2:
            blob = self.SM.tree_get(blob, [int(x, 16) for x in parts[2:]])
        return pac, ymks.read_bank(self.SM.unpack(blob))

    def bank_raw(self, path):
        """(pac, the bank's unpacked bytes) - YMBs banks, which ymks doesn't read"""
        pac = 'mpsp.pac' if path.startswith('MOTP') else 'm.pac'
        parts = path.split('/')
        blob = self.SM.entry_get(self.groups(pac), parts[0].ljust(4).encode(), parts[1].ljust(4).encode())
        if len(parts) > 2:
            blob = self.SM.tree_get(blob, [int(x, 16) for x in parts[2:]])
        return pac, self.SM.unpack(blob)

    def banks_of(self, mid):
        """[(path, kind, [(x, y)])]"""
        out = []
        for path, (kind, keys, size) in self.idx.items():
            ks = sorted({(k[1], k[2]) for k in keys if k[0] == mid})
            if ks:
                out.append((path, kind, ks))
        return out

    def records(self, mid):
        """the move's WAZA/DATA records: [(kind, group, record bytes, events bytes)]"""
        SM = self.SM
        kids = SM.waza_children(self.groups('misc.pac'))
        out = []
        for i, g_id, raw in SM.subgroups(kids[0]):
            for r in SM.exh_parse(raw, 36):
                if struct.unpack_from('<H', r, 0)[0] == mid:
                    out.append(('exh', i, r, b''))
        for i, g_id, raw in SM.subgroups(kids[1]):
            pr = SM.evt_parse(raw)
            if not pr:
                continue
            recs, ev = pr
            for r in recs:
                if struct.unpack_from('<H', r, 0)[0] == mid:
                    out.append(('evt', i, r, b''.join(SM.evt_list(r, ev))))
        for i, g_id, raw in SM.subgroups(kids[2]):
            for k in range(len(raw) // 8):
                r = raw[8 * k:8 * k + 8]
                if struct.unpack_from('<H', r, 0)[0] == mid:
                    out.append(('mbd', i, r, b''))
        return out

    def free_record(self, name=None, near=None):
        """a record 2011 keeps without motions (a cut move): the one named `name`, else the first. near: a
        template id - only records in its thousand (a taunt in 9000-9999: an in-match taunt on a record
        elsewhere, 3894, wasn't found - Sabu's 9040 was)"""
        free = [i for i in sorted(self.waze) if i not in self.moved and i > 1000]
        if near is not None and any(i // 1000 == near // 1000 for i in free):
            free = [i for i in free if i // 1000 == near // 1000]
        if name:
            for i in free:
                if norm(self.waze[i][1]) == norm(name):
                    return i, 'the free record with the same name'
        if not free:
            return None, ''
        # (another converted move should get another record: picked by the name, not the first free one;
        # the Mod Maker passes --id to keep clear of the packs already installed)
        h = zlib.crc32((name or '').encode('utf-8'))
        return free[h % len(free)], 'a free record picked by the name (%d free; --id to choose)' % len(free)


# --------------------------------------------------------------------------- convert

def start_gap(k0, k1):
    """the distance (x / z, root units) between two tracks' first roots"""
    r0 = next((k['root'] for k in k0 if k.get('root')), None)
    r1 = next((k['root'] for k in k1 if k.get('root')), None)
    if not r0 or not r1:
        return None
    return ((r0[0] - r1[0]) ** 2 + (r0[2] - r1[2]) ** 2) ** 0.5


def choose_template(g11, cat, frames, gap=None, allowed=None, paired=True):
    """the 2011 move in MVMT/<cat> (YMKs only, x0 y0 and x0 y1 both there) most like the source:
    the attacker / victim start distance (a running move needs a running template) and the length"""
    import ymks
    best = None
    for path, (kind, keys, size) in g11.idx.items():
        if not path.startswith('MVMT/' + (cat.strip() if cat else '')) or kind != 'YMKs':
            continue
        ids = {k[0] for k in keys if k[1] == 0 and k[2] == 0} & {k[0] for k in keys if k[1] == 0 and k[2] == 1}
        if not paired:   # (a one-person move - a strike: the victim reacts with its own motion)
            ids = {k[0] for k in keys if k[1] == 0 and k[2] == 0} - {k[0] for k in keys if k[2] == 1}
        if not ids:
            continue
        pac, bank = g11.bank(path)
        for e in bank.entries:
            if e['id'] in ids and e['x'] == 0 and e['y'] == 0:
                if e['id'] not in g11.waze:   # (motions without a move record: wazecopy needs one)
                    continue
                if allowed is not None and e['id'] not in allowed:
                    continue
                if any(kd == 'YMBs' for p, kd, ks in g11.banks_of(e['id'])):
                    continue
                score = abs(e['frames'] - frames) / 10.0
                if gap is not None:
                    v = bank.find(e['id'], 0, 1)
                    tg = start_gap(ymks.body_keys(e['data'], bank.T), ymks.body_keys(v['data'], bank.T)) if v else None
                    if tg is not None:
                        score += abs(tg - gap) / 100.0
                if best is None or score < best[0]:
                    best = (score, e['id'], path)
    return best[1] if best else None


def choose_template_by_length(g11, cat, frames):
    """(the old rule: the closest length only)"""
    best = None
    for path, (kind, keys, size) in g11.idx.items():
        if not path.startswith('MVMT/' + cat.strip()) or kind != 'YMKs':
            continue
        ids = {k[0] for k in keys if k[1] == 0 and k[2] == 0} & {k[0] for k in keys if k[1] == 0 and k[2] == 1}
        if not ids:
            continue
        pac, bank = g11.bank(path)
        for e in bank.entries:
            if e['id'] in ids and e['x'] == 0 and e['y'] == 0:
                if any(kd == 'YMBs' for p, kd, ks in g11.banks_of(e['id'])):
                    continue
                d = abs(e['frames'] - frames)
                if best is None or d < best[0]:
                    best = (d, e['id'], path)
    return best[1] if best else None


def retime(events, scale, n_len, old_id, new_id, limit=0x7f):
    """{frame: [event bytes]} moved to the new length; jumps (ops 49 / 50) to the template -> new id"""
    out = {}
    for f, evs in events:
        nf = min(n_len, int(round(f * scale)))
        for e in evs:
            e = bytearray(e)
            if e and e[0] in (49, 50) and len(e) >= 3 and struct.unpack_from('<H', e, 1)[0] == old_id:
                struct.pack_into('<H', e, 1, new_id)
            out.setdefault(nf, []).append(bytes(e))
    # (a frame holds at most 0x7f bytes of events (0x20 packed): what a shorter move piles up spills to the next frames)
    spill, res = [], {}
    for f in range(0, max(list(out) + [0]) + 64):
        cur = spill + out.get(f, [])
        spill, keep, size = [], [], 0
        for e in cur:
            if size + len(e) <= limit:
                keep.append(e)
                size += len(e)
            else:
                spill.append(e)
        if keep:
            res[f] = keep
        if not spill and f > max(list(out) + [0]):
            break
    return res


def convert(a, rep):
    import ymks
    import yanm_decode as Y
    import w13_retarget as WR
    sys.path.insert(0, HERE)
    sp = __import__('superstar_port')
    outdir = os.path.abspath(a['out'])
    os.makedirs(outdir, exist_ok=True)
    # (a['_g'] / a['_g11']: games already opened - moveset_port converts a superstar's moves in a row)
    g = a.get('_g')
    if g is None:
        src = sp.Src(a['src'], os.path.join(outdir, '.src_' + a['game']))
        progress(2, 'reading the game')
        g = open_game(a, src, os.path.join(outdir, '.work_cache'))
    if a['game'] in OLD:
        return convert0789(a, rep, outdir, g)
    try:
        mid = int(a['item'])
    except ValueError:
        raise BadInput('--item is a move id (from list)')
    idx = g.index()
    if mid not in idx:
        raise BadInput("%s has no motion for move %d" % (a['game'], mid))
    name = (a.get('name') or g.waze.get(mid) or 'MOVE %d' % mid)[:63]
    progress(10, 'decoding the motion')
    xs = sorted(idx[mid])
    cat0, path0 = idx[mid][xs[0]]
    bl = Y.decode_record(g.record(cat0, path0))
    bl = list(bl.values()) if isinstance(bl, dict) else list(bl)
    # blocks are named <phase 4 hex><person 4 hex>: 0000 0000/0001 a 2-frame set-up pose, 0010 0000/0001
    # the move's attacker / victim (0011.. variants), 0001-0005 0000 one-person parts. The move: the
    # first phase from 0010 with both people, else the longest one-person block.
    by = {}
    for b in bl:
        nm = str(b.get('block', '00000000')).rjust(8, '0')
        by.setdefault(nm[:4], {})[int(nm[4:], 16)] = b
    pairs = sorted(p for p, d in by.items() if 0 in d and 1 in d and d[0]['frames'] > 2)
    if pairs:
        phase = pairs[0]
        att, vic = by[phase][0], by[phase][1]
        rep['notes'].append('source blocks %s0000 / %s0001 (%d frames at 60 Hz)' % (phase, phase, att['frames']))
    else:
        att = max(bl, key=lambda b: b['frames'])
        vic = None
        rep['warnings'].append('no two-person block in the source motion: the attacker only, the victim stays the template\'s')
    nf = max(1, (att['frames'] - 1) // 2)
    progress(25, '2011 banks')
    g11 = a.get('_g11') or Game11(a['pac11'], os.path.join(outdir, '.work_cache'))
    R0 = WR.Retarget(bind='rest', twist=True, ik=a['game'] != 'wwe12')
    cat11 = CATS.get(cat0, '')   # ('': every category - 2K14's coded ones)
    gap = None
    if vic is not None:
        k0, k1 = R0.keys(att, 2), R0.keys(vic, 2)
        gap = ((k0[0][0][0] - k1[0][0][0]) ** 2 + (k0[0][0][2] - k1[0][0][2]) ** 2) ** 0.5
        rep['notes'].append('source start gap %.0f units' % gap)
    tid = int(a['template']) if a.get('template') else choose_template(g11, cat11, nf, gap)
    if not tid and not a.get('template'):   # (no two-person move of that category in 2011: any category)
        tid = choose_template(g11, '', nf, gap)
        cat11 = 'any'
    if not tid or tid not in g11.waze:
        raise BadInput('no 2011 template move (category %s); give --template' % cat11)
    if a.get('id'):
        new_id, why = int(a['id']), 'option'
    else:   # (a YMBs template - a taunt: a record in its thousand)
        ymb = any(kd == 'YMBs' for p, kd, ks in g11.banks_of(tid))
        new_id, why = g11.free_record(name, tid if ymb else None)
    if not new_id:
        raise RuntimeError('no free 2011 move record')
    rep['notes'].append('template %d %s (%s)' % (tid, g11.waze[tid][1], 'option' if a.get('template') else
                                                  'MVMT/%s, the closest start gap and length' % cat11.strip()))
    rep['notes'].append('2011 id %d (%s)' % (new_id, why))
    pack = re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')[:40] or 'move_%d' % mid
    pd = os.path.join(outdir, pack)
    os.makedirs(os.path.join(pd, 'motions'), exist_ok=True)
    progress(40, 'retargeting')
    R = WR.Retarget(bind='rest', twist=True, ik=a['game'] != 'wwe12')
    keys_of = {0: [R.keys(att, nf), R.keys(vic, nf) if vic is not None else None]}
    xs = [0]
    lines = ['wazecopy %d %d' % (new_id, tid), 'wazename %d %s' % (new_id, name)]
    progress(60, 'tracks')
    offset = None
    nmot = 0
    ymb_ns, ymb_solved = None, {}
    for path, kind, ks in g11.banks_of(tid):
        if kind == 'YMBs':   # (taunts, submissions: ADPCM channels with limb IK - ymbs_retarget)
            import ymbs
            import ymbs_retarget as YR
            pac, raw = g11.bank_raw(path)
            bank = ymbs.Bank(raw)
            ends = sorted({e[3] for e in bank.dir} | {len(raw) - bank.base})
            packed = path.startswith('MOTP')
            for x, y in ks:
                k = bank.find(tid, x, y)
                if k is None:
                    continue
                typ, nseg, itv, pad, f0, f1, f2, f3 = bank.hdr[k]
                blk = (att if y == 0 else vic) if x < 90 and y in (0, 1) else None
                data = None
                if blk is not None:
                    ymb_ns = ymb_ns or YR.nodes11()
                    try:
                        sample = YR.w13_sampler(R, blk)
                        if y not in ymb_solved:   # (the same motion in every bank: solved once)
                            ymb_solved[y] = YR.solve(ymb_ns, sample, nf)
                        data, frames = YR.build_motion(bank, ymb_ns, sample, nf, typ, ymb_solved[y])
                        worst = YR.check(bank, data, frames, typ, sample, ymb_ns)
                        rep['notes'].append('%s x%d y%d: YMBs motion, %d keys, worst joint %.2f units' %
                                            (path, x, y, frames, worst))
                        hdr = struct.pack('<BBBB4f', typ, 0, 1, 0, f0, f1, f2, f3)
                    except (StopIteration, IndexError) as ex:   # (a channel table this solver doesn't fill)
                        rep['warnings'].append('%s x%d y%d: the template\'s motion (%s)' % (path, x, y, ex or 'channels'))
                        data = None
                if data is None:   # (props, cameras, branches, the other person: the template's)
                    off = bank.dir[k][3]
                    data = raw[bank.base + off: bank.base + next(e for e in ends if e > off)]
                    frames = bank.dir[k][4]
                    hdr = raw[bank.base + 4 + 20 * k: bank.base + 24 + 20 * k]
                fn = 'motions/%d_%d_%d_%s.ymb' % (new_id, x, y, path.replace('/', '').lower())   # (per bank: channel tables differ)
                open(os.path.join(pd, fn), 'wb').write(data)
                lines.append('motion %s %s %d %d %d %d %s %s' % (pac, path, new_id, x, y, frames, fn, hdr.hex()))
                nmot += 1
            continue
        if kind != 'YMKs':
            rep['dropped'].append('bank %s (%s)' % (path, kind))
            continue
        pac, bank = g11.bank(path)
        packed = path.startswith('MOTP')
        for x, y in ks:
            e = bank.find(tid, x, y)
            if e is None:
                continue
            sx = x if x in keys_of else xs[0]
            src_keys = keys_of[sx][y] if y in (0, 1) and x < 90 else None
            if src_keys:
                tk = ymks.body_keys(e['data'], bank.T, packed)
                if offset is None and y == 0:   # (anchor: the template attacker's start, x / z)
                    r0 = next((k['root'] for k in tk if k.get('root')), None)
                    if r0:
                        offset = (r0[0] - src_keys[0][0][0], 0.0, r0[2] - src_keys[0][0][2])
                off = offset or (0.0, 0.0, 0.0)
                n = len(src_keys) - 1
                evs = retime([(k['frame'], k['events']) for k in tk if k.get('events')], n / float(max(1, e['frames'])),
                             n, tid, new_id, 0x20 if packed else 0x7f)
                ks_ = []
                for f, (root, ang) in enumerate(src_keys):
                    bones = {k: {ax: ang[k]['xyz'.index(ax)] for ax in ymks.axes_of(ymks.TYPES_M1[k])} for k in range(21)}
                    ks_.append(dict(frame=f, count=0, root=(root[0] + off[0], root[1], root[2] + off[2]), bones=bones,
                                    events=evs.pop(f, [])))
                for f, ev in sorted(evs.items()):
                    ks_.append(dict(frame=f, count=0, root=None, bones=None, events=ev))
                data, frames = ymks.build_motion(ks_, n + 1, packed=packed), n
            else:
                data, frames = e['data'], e['frames']   # (props, cameras, reversal branches: the template's)
            fn = 'motions/%d_%d_%d%s.ymk' % (new_id, x, y, '_p' if packed else '')
            open(os.path.join(pd, fn), 'wb').write(data)
            lines.append('motion %s %s %d %d %d %d %s' % (pac, path, new_id, x, y, frames, fn))
            nmot += 1
    for kind, grp, r, ev in g11.records(tid):
        r = bytearray(r)
        struct.pack_into('<H', r, 0, new_id)
        lines.append('%s %d %s%s' % (kind, grp, r.hex(), (' ' + ev.hex()) if kind == 'evt' else ''))
    open(os.path.join(pd, 'pack.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    rep['notes'].append('%d motions in %d banks, %d frames at 30 Hz' % (nmot, len(g11.banks_of(tid)), nf))
    rep['dropped'].append('finger poses (the template\'s); the source\'s own sounds and damage timing (the template\'s, retimed)')
    for sk in getattr(g, 'skipped', []):
        rep['warnings'].append('not readable, its moves left out: ' + sk)
    rep.update(ok=True, out=pd, folder=pd, id=new_id, name=name, template=tid)
    progress(100, 'done')
    return rep


def open_game(a, src, work=None):
    if a['game'] in OLD:
        return Game0789(src, work or os.path.join(HERE, 'work', 'm0789_' + a['game']))
    if a['game'] == 'wwe13':
        return Game13(src, a.get('svrmod'))
    if a['game'] == 'wwe2k14':
        return Game14(src, a.get('svrmod'))
    return Game12(src)


def cmd_list(a):
    sp = __import__('superstar_port')
    src = sp.Src(a['src'], os.path.join(a.get('cache') or os.path.join(HERE, 'work', 'src'), a['game']))
    g = open_game(a, src)
    idx = g.index()
    import svr10_superstar as SS
    w11 = SS.Game(a['pac11'], 11).waze()
    by11 = {}
    for i, (bits, nm) in w11.items():
        by11.setdefault(norm(nm), i)
    out = []
    for mid in sorted(idx):
        nm = g.waze.get(mid, '')
        cat = idx[mid][min(idx[mid])][0]
        i11 = by11.get(norm(nm)) if nm else None
        out.append(dict(key=str(mid), name=nm, category=cat.strip(), variants=len(idx[mid]), in_2011=i11))
    return out


def main(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(json.dumps({'error': 'usage: games | list --game G --src S | convert --game G --src S --item K --out D'}))
        return 2
    cmd = argv[1]
    a = dict(pac11=PAC11)
    i = 2
    while i < len(argv):
        k = argv[i]
        if k.startswith('--') and i + 1 < len(argv):
            a[k[2:].replace('-', '_')] = argv[i + 1]
            i += 2
        else:
            print(json.dumps({'error': 'unexpected argument: %s' % k}))
            return 2
    rep = dict(ok=False, out=None, folder=None, opens='moves', warnings=[], dropped=[], notes=[])
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
                if not a.get('item') or not a.get('out'):
                    raise BadInput('convert needs --item and --out')
                rep_path = os.path.join(os.path.abspath(a['out']), 'report.json')
                res = convert(a, rep)
        else:
            raise BadInput('unknown command %r' % cmd)
    except BadInput as e:
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 2
    except Exception as e:   # noqa: BLE001
        traceback.print_exc(file=sys.stderr)
        res = dict(rep, ok=False, error='%s: %s' % (type(e).__name__, e))
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
