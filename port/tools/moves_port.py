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
    others  "research" (WWE '13 / 2K14: the same motions inside xcompressed pacs; next)

How a move is made (the WWE '13 Mankind moves' method): the attacker (y 0) and the victim
(y 1) of every x the template has are the source's blocks 0 and 1, one key a frame at 30 Hz,
the attacker's start placed where the template's starts (both tracks moved alike: the victim
keeps the source's place relative to the attacker); prop and camera tracks and reversal
branches (x 90-92) are the template's. The template's in-stream events are moved in time
with the new length, its jumps to itself point to the new move. Its record (pack `wazecopy`),
name (`wazename`), per-key EXH / sound / MBD records (exh / evt / mbd) go with it. Not
carried: finger poses (the template's hands), the source's own sounds and damage timing
(the template's, retimed). Submission and taunt templates (YMBs banks) are left out for now.
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
    dict(id='wwe13', name="WWE '13", platform='x360', disc_hint='545108B4', status='research'),
    dict(id='wwe2k14', name='WWE 2K14', platform='x360', disc_hint='545408B2', status='research'),
    dict(id='svr2010', name='WWE SmackDown vs. Raw 2010', platform='x360', disc_hint='54510844', status='research'),
]
READY = {g['id'] for g in GAMES if g['status'] == 'ready'}
# WWE '12 ZMVM category -> SvR 2011 MVMT bank (moveset menu categories)
CATS = {'STAT': 'STAT', 'BASE': 'BASE', 'STN1': 'STN1', 'STN2': 'STN2', 'GRND': 'GRND', 'CORN': 'CORN',
        'CETB': 'CETB', 'ROPE': 'ROPE', 'APRO': 'APRO', 'RUN ': 'RUN ', 'TEAM': 'TEAM', 'FINI': 'FINI',
        'SPE1': 'STN1', 'SPE2': 'STN2', 'ANAS': 'ANAS', 'RUMB': 'RUMB', 'LADR': 'LADR', 'CELL': 'CELL',
        'ABIL': 'ABIL', 'DIVI': 'CORN', 'APR2': 'APRO', 'ANA2': 'ANAS', 'WEAP': 'STAT'}


class BadInput(Exception):
    pass


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def progress(pct, text):
    print('progress %d %s' % (pct, text), file=sys.stderr, flush=True)


def norm(s):
    s = s.lower().replace('_', ' ').strip()
    return re.sub(r'[^a-z0-9]', '', re.sub(r'^\d+ ', '', s))


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
        g = svrfmt.epac_read(open(src.get('pac/misc.pac'), 'rb').read())[1]
        raw = unz(dict(((t, n), b) for t, es in g for n, b in es)[(b'MOVS', b'WAZE')])
        n = struct.unpack_from('<I', raw, 4)[0]
        self.waze = {}
        for k in range(n):
            r = raw[0x430 + 160 * k: 0x430 + 160 * (k + 1)]
            if len(r) < 160 or not r.strip(b'\0'):
                continue
            mid = struct.unpack_from('<H', r, 0x90)[0]
            self.waze.setdefault(mid, r[0x10:0x50].split(b'\0')[0].decode('latin1').strip())
        self._m = None
        self._index = None

    def _groups(self):
        if self._m is None:
            self._m = svrfmt.epac_read(open(self.src.get('pac/m.pac'), 'rb').read())[1]
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
        b = dict(next(es for t, es in self._groups() if t == b'ZMVM'))[cat.encode('latin1')]
        raw = unz(b)
        for i in path:
            raw = unz(dict(svrfmt.pach_read(raw))[i])
        return raw


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

    def free_record(self, name=None):
        """a record 2011 keeps without motions (a cut move): the one named `name`, else the first"""
        free = [i for i in sorted(self.waze) if i not in self.moved and i > 1000]
        if name:
            for i in free:
                if norm(self.waze[i][1]) == norm(name):
                    return i, 'the free record with the same name'
        return (free[0], 'the first free record') if free else (None, '')


# --------------------------------------------------------------------------- convert

def start_gap(k0, k1):
    """the distance (x / z, root units) between two tracks' first roots"""
    r0 = next((k['root'] for k in k0 if k.get('root')), None)
    r1 = next((k['root'] for k in k1 if k.get('root')), None)
    if not r0 or not r1:
        return None
    return ((r0[0] - r1[0]) ** 2 + (r0[2] - r1[2]) ** 2) ** 0.5


def choose_template(g11, cat, frames, gap=None):
    """the 2011 move in MVMT/<cat> (YMKs only, x0 y0 and x0 y1 both there) most like the source:
    the attacker / victim start distance (a running move needs a running template) and the length"""
    import ymks
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
    src = sp.Src(a['src'], os.path.join(outdir, '.src_' + a['game']))
    progress(2, 'reading the game')
    g = Game12(src)
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
    g11 = Game11(a['pac11'], os.path.join(outdir, '.work_cache'))
    R0 = WR.Retarget(bind='rest', twist=True, ik=False)
    cat11 = CATS.get(cat0, 'STN1')
    gap = None
    if vic is not None:
        k0, k1 = R0.keys(att, 2), R0.keys(vic, 2)
        gap = ((k0[0][0][0] - k1[0][0][0]) ** 2 + (k0[0][0][2] - k1[0][0][2]) ** 2) ** 0.5
        rep['notes'].append('source start gap %.0f units' % gap)
    tid = int(a['template']) if a.get('template') else choose_template(g11, cat11, nf, gap)
    if not tid or tid not in g11.waze:
        raise BadInput('no 2011 template move (category %s); give --template' % cat11)
    if a.get('id'):
        new_id, why = int(a['id']), 'option'
    else:
        new_id, why = g11.free_record(name)
    if not new_id:
        raise RuntimeError('no free 2011 move record')
    rep['notes'].append('template %d %s (%s)' % (tid, g11.waze[tid][1], 'option' if a.get('template') else
                                                  'MVMT/%s, the closest start gap and length' % cat11.strip()))
    rep['notes'].append('2011 id %d (%s)' % (new_id, why))
    pack = re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')[:40] or 'move_%d' % mid
    pd = os.path.join(outdir, pack)
    os.makedirs(os.path.join(pd, 'motions'), exist_ok=True)
    progress(40, 'retargeting')
    R = WR.Retarget(bind='rest', twist=True, ik=False)
    keys_of = {0: [R.keys(att, nf), R.keys(vic, nf) if vic is not None else None]}
    xs = [0]
    lines = ['wazecopy %d %d' % (new_id, tid), 'wazename %d %s' % (new_id, name)]
    progress(60, 'tracks')
    offset = None
    nmot = 0
    for path, kind, ks in g11.banks_of(tid):
        if kind != 'YMKs':
            rep['dropped'].append('bank %s (YMBs: submissions / taunts not done yet)' % path)
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
    rep.update(ok=True, out=pd, folder=pd, id=new_id, name=name, template=tid)
    progress(100, 'done')
    return rep


def cmd_list(a):
    sp = __import__('superstar_port')
    src = sp.Src(a['src'], os.path.join(a.get('cache') or os.path.join(HERE, 'work', 'src'), a['game']))
    g = Game12(src)
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
