"""SvR 2008 entrance (EVP + EVT, Xbox 360) -> SvR 2011 entrance.

Format notes: docs/SVR08_ENTRANCES.md.

  python svr08_entrance.py validate                       4071 -> 506 report (needs sandman_evt.pkl)
  python svr08_entrance.py selftest                       writer round-trips on every 2011 entrance
                                                     (needs ..\\..\\corpus_evt.pkl, see corpus.py)
  python svr08_entrance.py convert EVP08 EVT08 NUMBER OUT_EVP OUT_EVT [--ref REF08_EVP REF08_EVT REF11_EVP REF11_EVT]
                      [--parts DIR] [--no-dup6x]

EVP ("0FOP"/"EV", big-endian, pointers relative to file offset 8, POF0 table at the end):
  0x00 '0FOP' u32 size | 0x08 'EV\\0\\0' u32 size (same) | 0x10 u32 0x10000 | 0x14 ptr -> 0x20
  0x18 u32 0x10000 if a tail block exists else 0 | 0x1C ptr -> tail (0 = none)
  0x20 u16 entrance number, name (30 bytes) | 0x40 u32 object count | 0x44 u32 ? | 0x48 ptr -> table (0x4C)
  object table: 2008 28 bytes {u16 idx, u16 model, u8 type[8], u32 -1, u32 0, u16 slots, u16 0, ptr block}
                2011 32 bytes {u32 idx, u32 model, ...same...}
  block: `slots` x 212-byte slots: {u16 slot, u8 on, u8 0, u32 EVT id, u32 slot, 48 x u32 params,
          u16 parent object (9999 = none), u16 0}
  tail: {u16 number, u16 n, ptr recs}, n x {u16 type, u16 0, u32 100, u32 m, ptr items},
        m x {u8 kind, u24 0, ptr value}, values (kind 2 = u32, kind 1/3 = u16)
EVT: 2008 = SvR 2011 YMKs bank without its 16-byte header; T differs (2011 adds ops, 151 is 3 bytes).
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ymks  # noqa: E402

ID_SHIFT = 25000                     # EVT ids: 2008 0.. -> 2011 25000..
RT_2011 = 55438460                   # dir 4th field (overwritten at runtime; 506's value)
SLOT = 212
NONE16 = 9999
T11 = bytes.fromhex(                 # 2011 event-size table (variant used by 506 and 480 other banks)
    '010203050700020100000300000000000404060008060308000200040000000001010101010c0101020105050a030b00'
    '000304040102040601010302030302050603020a03010102070c0b0b04040309020203000205010201010200020601'
    '020101010101010101010000040401030203020201030402010202030103020203020302030302010306050304020c'
    '020202020202020404030402010201020305010102020202030202020204050202020200000402010101020201010101'
    '010102030000000000000000000000000000020101020101010000000000000000000203020200000000000000000000'
    '000000000000000000000000000000000000')
assert len(T11) == 256

TYPE_CHAR = (0x01000000, 0)          # object type bytes (two big-endian u32)
TYPE_PROP = (0x00000100, 0)
TYPE_GLOBAL = (0, 0x00010000)
TYPE_LIGHTS = ((0x00000001, 0), (0, 0x01000000))   # [L] light-like objects (models 0/1/2)        # tracks excluded from CAE parts (x 10..14 / 60..64 effects)
PROP_MAP = {2029: 5009}              # World Heavyweight Title - Rolled -> Folded2 (Yuke's, 56/67 pairs)
GENERIC_CHAR_08, GENERIC_CHAR_11 = 3902, 13902
PREVIEW_CHAR_11 = 16102              # 2011 placeholder body for an entrant with no 2011 model


# ============================================================================ EVP

def _u32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def pof0_read(d):
    tot = _u32(d, 0xC)
    p = 8 + tot
    assert d[p:p + 4] == b'POF0', 'POF0 not at %x' % p
    n = _u32(d, p + 4)
    q, end, off, out = p + 8, p + 8 + n, 8, []
    while q < end:
        b = d[q]
        t = b >> 6
        if t == 0:
            break
        if t == 1:
            v = b & 0x3f; q += 1
        elif t == 2:
            v = ((b & 0x3f) << 8) | d[q + 1]; q += 2
        else:
            v = ((b & 0x3f) << 24) | (d[q + 1] << 16) | (d[q + 2] << 8) | d[q + 3]; q += 4
        off += v * 4
        out.append(off)
    return out


def pof0_write(ptrs):
    out = bytearray()
    prev = 8
    for o in sorted(ptrs):
        v = (o - prev) // 4
        assert v > 0 and (o - prev) % 4 == 0
        if v < 0x40:
            out.append(0x40 | v)
        elif v < 0x4000:
            out += bytes([0x80 | (v >> 8), v & 0xff])
        else:
            out += bytes([0xC0 | (v >> 24), (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff])
        prev = o
    out += b'\0' * (-len(out) % 4)
    return b'POF0' + struct.pack('>I', len(out)) + bytes(out)


def parse_evp(d, game):
    """-> dict(num, name, f10, f18, f44, objs=[dict(idx, model, t0, t1, w4, w5, nslots, lo, slots=[212B])],
    tail=None | dict(num, recs=[(typ, z, v, [(kind, low24, value)])]))"""
    assert d[:4] == b'0FOP' and d[8:10] == b'EV'
    e = dict(num=struct.unpack_from('>H', d, 0x20)[0], name=d[0x22:0x40], f10=_u32(d, 0x10),
             f18=_u32(d, 0x18), f44=_u32(d, 0x44), objs=[], tail=None, game=game)
    n = _u32(d, 0x40)
    t = _u32(d, 0x48) + 8
    es = 28 if game == '2008' else 32
    for i in range(n):
        q = t + es * i
        if game == '2008':
            idx, model = struct.unpack_from('>HH', d, q); q += 4
        else:
            idx, model = struct.unpack_from('>II', d, q); q += 8
        t0, t1, w4, w5, ns, bp = struct.unpack_from('>6I', d, q)
        bp += 8
        nslots = ns >> 16
        slots = [d[bp + SLOT * k:bp + SLOT * (k + 1)] for k in range(nslots)]
        e['objs'].append(dict(idx=idx, model=model, t0=t0, t1=t1, w4=w4, w5=w5, nslots=nslots,
                              lo=ns & 0xffff, slots=slots))
    tp = _u32(d, 0x1C)
    if tp:
        tp += 8
        num, cnt = struct.unpack_from('>HH', d, tp)
        lp = _u32(d, tp + 4) + 8
        recs = []
        for i in range(cnt):
            typ, z, v, m, ip = struct.unpack_from('>HHIII', d, lp + 16 * i)
            ip += 8
            items = []
            for j in range(m):
                kind, vp = struct.unpack_from('>II', d, ip + 8 * j)
                vp += 8
                k = kind >> 24
                val = _u32(d, vp) if k == 2 else struct.unpack_from('>H', d, vp)[0]
                items.append((k, kind & 0xffffff, val))
            recs.append((typ, z, v, items))
        e['tail'] = dict(num=num, recs=recs)
    return e


def build_evp(e, game='2011'):
    """2011 layout (game='2008' writes the 2008 layout, used only to self-test the parser)."""
    n = len(e['objs'])
    es = 32 if game == '2011' else 28
    ptrs = [0x14, 0x1C, 0x48]
    body = bytearray(0x4C + es * n)
    pos = len(body)
    blocks = bytearray()
    for i, o in enumerate(e['objs']):
        q = 0x4C + es * i
        if game == '2011':
            struct.pack_into('>II', body, q, o['idx'], o['model']); q += 8
        else:
            struct.pack_into('>HH', body, q, o['idx'], o['model']); q += 4
        assert len(o['slots']) == o['nslots']
        struct.pack_into('>6I', body, q, o['t0'], o['t1'], o['w4'], o['w5'],
                         (o['nslots'] << 16) | o['lo'], pos + len(blocks) - 8)
        ptrs.append(q + 20)
        for s in o['slots']:
            assert len(s) == SLOT
            blocks += s
    body += blocks
    tail = e.get('tail')
    if tail:
        tp = len(body)
        recs = tail['recs']
        tb = bytearray(struct.pack('>HHI', tail['num'], len(recs), tp + 8 - 8))
        ptrs.append(tp + 4)
        rec_at = tp + 8
        list_at = rec_at + 16 * len(recs)
        lists, la = [], list_at
        for r in recs:
            lists.append(la)
            la += 8 * len(r[3])
        vals = bytearray()
        val_base = la
        item_ptr = {}
        for ri, r in enumerate(recs):
            order = list(range(len(r[3])))
            if game == '2011':
                order.sort(key=lambda j: r[3][j][0])      # 2011 writes kind 1 values before kind 2
            for j in order:
                k, low, val = r[3][j]
                if k == 2:
                    vals += b'\0' * (-(val_base + len(vals)) % 4)
                    item_ptr[(ri, j)] = val_base + len(vals)
                    vals += struct.pack('>I', val)
                else:
                    vals += b'\0' * (-(val_base + len(vals)) % 2)
                    item_ptr[(ri, j)] = val_base + len(vals)
                    vals += struct.pack('>H', val)
        for ri, r in enumerate(recs):
            tb += struct.pack('>HHIII', r[0], r[1], r[2], len(r[3]), lists[ri] - 8)
            ptrs.append(rec_at + 16 * ri + 12)
        for ri, r in enumerate(recs):
            for j, (k, low, val) in enumerate(r[3]):
                tb += struct.pack('>II', (k << 24) | low, item_ptr[(ri, j)] - 8)
                ptrs.append(lists[ri] + 8 * j + 4)
        tb += vals
        body += tb
        struct.pack_into('>I', body, 0x1C, tp - 8)
    body += b'\0' * (-(len(body) - 8) % 16)
    size = len(body) - 8
    body[0:4] = b'0FOP'
    struct.pack_into('>I', body, 4, size)
    body[8:12] = b'EV\0\0'
    struct.pack_into('>I', body, 0xC, size)
    struct.pack_into('>III', body, 0x10, e['f10'], 0x18, e['f18'])
    struct.pack_into('>H', body, 0x20, e['num'])
    body[0x22:0x40] = e['name']
    struct.pack_into('>III', body, 0x40, n, e['f44'], 0x44)
    return bytes(body) + pof0_write(ptrs)


def slot_fields(s):
    w = struct.unpack('>53I', s)
    return dict(slot=w[0] >> 16, on=(w[0] >> 8) & 0xff, evt_id=w[1], x=w[2], params=w[3:52],
                parent=w[52] >> 16)


def convert_evp(e8, number, obj0_model=PREVIEW_CHAR_11, prop_map=PROP_MAP, log=None):
    """2008 EVP dict -> 2011 EVP dict (pure structural rules, see RE_ENTRANCE08.md)."""
    log = log if log is not None else []
    e = dict(e8)
    e['num'] = number
    e['f44'] = 0                                   # 52/52 Yuke's pairs: 2008 1/2/0 -> 2011 0
    e['f18'] = 0x10000 if e8['tail'] else 0
    objs = []
    for i, o in enumerate(e8['objs']):
        o = dict(o)
        typ = (o['t0'], o['t1'])
        if typ == TYPE_CHAR:
            if i == 0:
                new = obj0_model
            elif o['model'] == GENERIC_CHAR_08 and 1 <= i <= 6 and obj0_model == PREVIEW_CHAR_11:
                new = PREVIEW_CHAR_11              # Yuke's: extras 1..6 follow the preview body
            elif o['model'] == GENERIC_CHAR_08:
                new = GENERIC_CHAR_11
            else:
                new = o['model'] + 10000 if o['model'] < 10000 else o['model']
                log.append('object %d: char model %d -> %d (check it exists in 2011)' % (i, o['model'], new))
            o['model'] = new
        elif typ == TYPE_PROP and o['model'] in prop_map:
            log.append('object %d: prop %d -> %d' % (i, o['model'], prop_map[o['model']]))
            o['model'] = prop_map[o['model']]
        slots = []
        for s in o['slots']:
            s = bytearray(s)
            struct.pack_into('>I', s, 4, _u32(s, 4) + ID_SHIFT)
            slots.append(bytes(s))
        o['slots'] = slots
        objs.append(o)
    e['objs'] = objs
    if e8['tail']:
        recs = []
        for typ, z, v, items in e8['tail']['recs']:
            items = list(items)
            if typ == 1 and [k for k, _, _ in items] == [2]:
                items += [(1, 0, 0), (1, 0, 0)]    # 2011 music record: 2 extra u16 params (all 216 tails)
            recs.append((typ, z, v, items))
        e['tail'] = dict(num=number, recs=recs)
    e['game'] = '2011'
    return e


# ============================================================================ EVT

def read_evt(raw, game):
    if game == '2008':
        assert raw[:4] == b'\x01\x02\x03\x05', raw[:4]
        raw = b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + raw
    return ymks.read_bank(raw)


def _lo193(ev):
    return ev[0] == 193 and 0x30 <= ev[1] <= 0x3f


def convert_evt(raw08, dup6x=True, log=None):
    """2008 EVT bytes -> 2011 EVT bytes."""
    log = log if log is not None else []
    b = read_evt(raw08, '2008')
    T8 = b.T
    for op in range(256):
        if T8[op] and T8[op] != T11[op]:
            log.append('op %d size %d (2008) vs %d (2011)' % (op, T8[op], T11[op]))
    used = set()
    dropped = 0
    ents = []
    for e in b.entries:
        e = dict(e)
        e['id'] += ID_SHIFT
        e['rt'] = RT_2011
        data = e['data']
        if data and not e.get('share'):
            st = ymks.parse_stream(data)
            items = []
            changed = False
            for it in st.items:
                if it[0] != 'rec':
                    items.append(it)
                    continue
                ev, pose = ymks.split_record(it[1])
                if ev:
                    evl, ok = ymks.parse_events(ev, T8)
                    for op, bb in evl:
                        used.add(op)
                    if ok and e['y'] == 0 and any(op is not None and _lo193(bb) for op, bb in evl):
                        # 2011 keeps 193 '3x' events only on the y1..3 tracks
                        keep = [bb for op, bb in evl if not _lo193(bb)]
                        dropped += len(evl) - len(keep)
                        rec = ymks.join_record(b''.join(keep), pose)
                        items.append(('rec', rec, len(rec) > 0x9f))
                        changed = True
                        continue
                items.append(it)
            if changed:
                e['data'] = ymks.build_stream(ymks.Stream(items))
        ents.append(e)
    bad = sorted(op for op in used if op is not None and T8[op] != T11[op])
    if bad:
        log.append('WARNING: ops with changed size used: %s' % bad)
    log.append('dropped %d op-193 3x events from y=0 tracks' % dropped)
    # re-link shared entries to their (possibly rebuilt) predecessor
    for i, e in enumerate(ents):
        if e.get('share') and i:
            e['data'] = ents[i - 1]['data']
    bank = ymks.Bank(b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + T11, ents)
    bank.lead = b.lead
    if dup6x:
        added = 0
        ids = sorted(set(e['id'] for e in bank.entries))
        for i in ids:
            mine = [e for e in bank.entries if e['id'] == i]
            xs = set(e['x'] for e in mine)
            if not any(11 <= x <= 14 for x in xs):
                continue                           # (only the 'global' effect objects use x 11..14)
            have = set((e['x'], e['y']) for e in mine)
            for e in mine:
                if 10 <= e['x'] <= 14 and e['y'] < 50 and (e['x'] + 50, e['y']) not in have:
                    ymks.insert_motion(bank, i, e['x'] + 50, e['y'], e['data'], e['frames'], RT_2011)
                    added += 1
        log.append('x+50 copies of x 10..14 tracks: %d' % added)
    return ymks.write_bank(bank)


# ============================================================================ CAE parts

def cae_parts(evp11, evt11, number):
    """Create-An-Entrance parts 10000+1000k+number (k = 0..4, segment x = 5k), the way 2011 ships them:
    EVP: every object keeps slots 0..5k (slots before 5k: 'on' byte 0, parent 9999), no tail;
    EVT: the full bank's x = 5k tracks except 'global' (type 0/0x10000) objects."""
    e = parse_evp(evp11, '2011')
    b = ymks.read_bank(evt11)
    glob = set(ID_SHIFT + i for i, o in enumerate(e['objs']) if (o['t0'], o['t1']) == TYPE_GLOBAL)
    out = []
    for k in range(5):
        ns = 5 * k + 1
        p = dict(e)
        p['num'] = 10000 + 1000 * k + number
        p['tail'] = None
        p['f18'] = 0
        objs = []
        for o in e['objs']:
            o = dict(o)
            sl = []
            for j in range(ns):
                if j == ns - 1:
                    sl.append(o['slots'][j])
                    continue
                s = bytearray(o['slots'][j])      # earlier slots: kept, switched off, detached
                s[2] = 0
                struct.pack_into('>H', s, SLOT - 4, NONE16)
                if (o['t0'], o['t1']) in TYPE_LIGHTS:
                    s[12:SLOT - 4] = bytes(SLOT - 16)   # light keys of earlier segments dropped
                sl.append(bytes(s))
            o['slots'] = sl
            o['nslots'] = ns
            objs.append(o)
        p['objs'] = objs
        ents = [dict(x) for x in b.entries if x['x'] == 5 * k and x['id'] not in glob]
        for i, x in enumerate(ents):
            if x.get('share') and (i == 0 or ents[i - 1]['data'] is not x['data']):
                x['share'] = False
        pb = ymks.Bank(b.head, ents)
        out.append((p['num'], build_evp(p), ymks.write_bank(pb)))
    return out


# ============================================================================ edit transfer

def transfer_edits(new11, src08, ref08, ref11, log):
    """Apply Yuke's own 2008->2011 hand edits of a reference entrance (e.g. 4071 -> 506) to a
    converted entrance, only where the 2008 source is byte-identical to the 2008 reference.
    EVP: object model per object (matched by type+2008 model), slot params/parent per slot.
    Returns the edited EVP dict."""
    used = set()
    for i, o in enumerate(new11['objs']):
        s8 = src08['objs'][i]
        cand = [j for j, r in enumerate(ref08['objs']) if j not in used and
                (r['t0'], r['t1'], r['model']) == (s8['t0'], s8['t1'], s8['model']) and r['nslots'] == s8['nslots']]
        # prefer the same index, then the first with identical slots (ids aside)
        def same_slots(j):
            a = [x[:4] + x[8:] for x in ref08['objs'][j]['slots']]
            bb = [x[:4] + x[8:] for x in s8['slots']]
            return a == bb
        j = i if i in cand else (next((c for c in cand if same_slots(c)), cand[0] if cand else None))
        if j is None:
            log.append('object %d: no reference object' % i)
            continue
        used.add(j)
        r8, r11 = ref08['objs'][j], ref11['objs'][j]
        if o['model'] != r11['model']:
            log.append('object %d: model %d -> %d (as reference object %d)' % (i, o['model'], r11['model'], j))
            o['model'] = r11['model']
        slots = list(o['slots'])
        for k in range(min(len(slots), len(r8['slots']))):
            a8, a11, mine8 = r8['slots'][k], r11['slots'][k], s8['slots'][k]
            if a8[:4] + a8[8:] != mine8[:4] + mine8[8:]:
                continue                       # this slot differs from the reference in 2008: keep
            want = bytearray(slots[k])
            want[:4] = a11[:4]
            want[8:] = a11[8:]                 # (bytes 4..8 = this entrance's EVT id)
            if bytes(want) != slots[k]:
                log.append('object %d slot %d: reference edit applied (from object %d)' % (i, k, j))
            slots[k] = bytes(want)
        o['slots'] = slots
    return new11


def transfer_tracks(evt11_bytes, src08_raw, ref08_raw, ref11_raw, log, src08_evp=None, ref08_evp=None):
    """EVT: where a 2008 track of the source is byte-identical to the reference's 2008 track with the
    same object (by object match) and key, take the reference's 2011 track; also bring reference-only
    2011 tracks of those keys' siblings (e.g. added y1..3) only when the whole (object, x) group matches."""
    b = ymks.read_bank(evt11_bytes)
    s8 = read_evt(src08_raw, '2008')
    r8 = read_evt(ref08_raw, '2008')
    r11 = ymks.read_bank(ref11_raw)
    omap = {}
    if src08_evp and ref08_evp:
        used = set()
        for i, o in enumerate(src08_evp['objs']):
            for j, r in enumerate(ref08_evp['objs']):
                if j not in used and (r['t0'], r['t1'], r['model']) == (o['t0'], o['t1'], o['model']):
                    if i == j or j not in omap.values():
                        omap[i] = j; used.add(j); break
    groups8s = {}
    for e in s8.entries:
        groups8s.setdefault((e['id'], e['x']), {})[e['y']] = e
    groups8r = {}
    for e in r8.entries:
        groups8r.setdefault((e['id'], e['x']), {})[e['y']] = e
    groups11r = {}
    for e in r11.entries:
        groups11r.setdefault((e['id'] - ID_SHIFT, e['x']), {})[e['y']] = e
    n = 0
    for (i, x), g in sorted(groups8s.items()):
        j = omap.get(i, i)
        gr = groups8r.get((j, x))
        if not gr or set(gr) != set(g):
            continue
        if any(gr[y]['data'] != g[y]['data'] or gr[y]['frames'] != g[y]['frames'] for y in g):
            continue
        todo = [(x, y, e) for y, e in groups11r.get((j, x), {}).items()]
        if 10 <= x <= 14:                      # Yuke's x+50 variants of these tracks
            todo += [(x + 50, y, e) for y, e in groups11r.get((j, x + 50), {}).items()
                     if y in g and (j, x + 50) not in groups8r]
        for xx, y, e in sorted(todo, key=lambda t: (t[0], t[1])):
            x_save, x = x, xx
            cur = b.find(i + ID_SHIFT, x, y)
            if cur is not None and cur['data'] == e['data']:
                x = x_save
                continue
            ymks.insert_motion(b, i + ID_SHIFT, x, y, e['data'], e['frames'], RT_2011)
            n += 1
            log.append('EVT (%d,%d,%d): reference 2011 track used (ref object %d)' % (i, x, y, j))
            x = x_save
    log.append('EVT reference tracks applied: %d' % n)
    return ymks.write_bank(b)


# ============================================================================ CLI

def diff_report(a, b, label):
    if a == b:
        return '%s: identical (%d bytes)' % (label, len(a))
    n = sum(1 for i in range(min(len(a), len(b))) if a[i] != b[i])
    return '%s: %d vs %d bytes, %d differing bytes in the common length' % (label, len(a), len(b), n)


def evp_diff(a, b):
    """structured EVP diff (both 2011) -> lines"""
    A, B = parse_evp(a, '2011'), parse_evp(b, '2011')
    out = []
    for k in ('num', 'name', 'f10', 'f18', 'f44'):
        if A[k] != B[k]:
            out.append('header %s: %r vs %r' % (k, A[k], B[k]))
    if len(A['objs']) != len(B['objs']):
        out.append('object count %d vs %d' % (len(A['objs']), len(B['objs'])))
    for i, (x, y) in enumerate(zip(A['objs'], B['objs'])):
        for k in ('idx', 'model', 't0', 't1', 'w4', 'w5', 'nslots', 'lo'):
            if x[k] != y[k]:
                out.append('object %d %s: %r vs %r' % (i, k, x[k], y[k]))
        for s, (p, q) in enumerate(zip(x['slots'], y['slots'])):
            if p != q:
                fp, fq = slot_fields(p), slot_fields(q)
                d = [k for k in ('slot', 'on', 'evt_id', 'x', 'parent') if fp[k] != fq[k]]
                pw = [w for w in range(49) if fp['params'][w] != fq['params'][w]]
                out.append('object %d slot %d: %s%s' % (i, s, ' '.join('%s %r->%r' % (k, fp[k], fq[k]) for k in d),
                           (' params %s' % ', '.join('w%d %08x->%08x' % (w + 3, fp['params'][w], fq['params'][w]) for w in pw)) if pw else ''))
    if A['tail'] != B['tail']:
        out.append('tail: %r vs %r' % (A['tail'], B['tail']))
    return out


def evt_diff(a, b):
    A, B = ymks.read_bank(a), ymks.read_bank(b)
    out = []
    if A.head != B.head:
        out.append('head differs')
    ka = {(e['id'], e['x'], e['y']): e for e in A.entries}
    kb = {(e['id'], e['x'], e['y']): e for e in B.entries}
    for k in sorted(set(ka) | set(kb)):
        x, y = ka.get(k), kb.get(k)
        if x is None:
            out.append('%s only in 2011 (%d frames, %d bytes)' % (k, y['frames'], len(y['data'])))
        elif y is None:
            out.append('%s only in converted' % (k,))
        elif x['data'] != y['data'] or x['frames'] != y['frames'] or x['rt'] != y['rt']:
            ra = dict(ymks.parse_stream(x['data']).records())
            rb = dict(ymks.parse_stream(y['data']).records())
            ev = pose = 0
            evs = []
            for f in sorted(set(ra) | set(rb)):
                p, q = ra.get(f, b''), rb.get(f, b'')
                if p == q:
                    continue
                e1, p1 = ymks.split_record(p)
                e2, p2 = ymks.split_record(q)
                if e1 != e2:
                    ev += 1
                    l1 = [bb.hex() for op, bb in ymks.parse_events(e1, A.T)[0]] if e1 else []
                    l2 = [bb.hex() for op, bb in ymks.parse_events(e2, B.T)[0]] if e2 else []
                    evs.append('f%d %s->%s' % (f, '+'.join(l1) or '-', '+'.join(l2) or '-'))
                if p1 != p2:
                    pose += 1
            out.append('%s: %d vs %d bytes, %d frames, event diffs in %d records, pose diffs in %d records%s' % (
                k, len(x['data']), len(y['data']), y['frames'], ev, pose,
                (' [' + '; '.join(evs[:6]) + (' ...' if len(evs) > 6 else '') + ']') if evs else ''))
    if [(e['id'], e['x'], e['y']) for e in A.entries] != [(e['id'], e['x'], e['y']) for e in B.entries] and \
            set(ka) == set(kb):
        out.append('directory order differs')
    return out


def cmd_validate():
    import pickle
    d = pickle.load(open(os.path.join(HERE, 'sandman_evt.pkl'), 'rb'))
    p8, t8 = d[('2008', b'EVP2', b'4071')], d[('2008', b'EVT2', b'4071')]
    p11, t11 = d[('2011', b'EVPE', b'506')], d[('2011', b'EVTE', b'506')]
    log = []
    e8 = parse_evp(p8, '2008')
    assert build_evp(e8, '2008')[:len(p8)] == p8[:len(build_evp(e8, '2008'))], '2008 EVP parse/rebuild mismatch'
    assert build_evp(parse_evp(p11, '2011')) == p11, '2011 EVP writer does not round-trip 506'
    cp = build_evp(convert_evp(e8, 506, log=log))
    ct = convert_evt(t8, log=log)
    print('\n'.join(log))
    print(diff_report(cp, p11, 'EVP 4071->506'))
    for l in evp_diff(cp, p11):
        print('  ', l)
    print(diff_report(ct, t11, 'EVT 4071->506'))
    for l in evt_diff(ct, t11):
        print('  ', l)


def _no_rt(evt):
    """EVT bytes with the directory rt fields zeroed (the game overwrites them at runtime)"""
    b = bytearray(evt)
    n = struct.unpack_from('<I', b, 0x110)[0]
    for i in range(n):
        b[0x114 + 16 * i + 12:0x114 + 16 * i + 16] = bytes(4)
    return bytes(b)


def cmd_selftest():
    import pickle
    C = pickle.load(open(os.path.join(HERE, '..', '..', 'corpus_evt.pkl'), 'rb'))
    P = {(g, t[:3], n.decode()): u for g, f, t, n, u in C if not n.startswith(b'9999')}
    ok = bad = 0
    for (g, t, n), u in sorted(P.items()):
        if t != b'EVP':
            continue
        try:
            r = build_evp(parse_evp(u, g), g)
            good = r == u if g == '2011' else r == u[:len(r)] and not u[len(r):].strip(b'\0')
        except Exception as ex:
            good = False
        ok += good
        bad += not good
        if not good and bad < 10:
            print('round-trip fail', g, n)
    print('EVP round-trip: %d ok, %d fail' % (ok, bad))
    ok = bad = 0
    for (g, t, n), u in sorted(P.items()):
        if g != '2011' or t != b'EVP' or int(n) >= 10000 or ('2011', b'EVP', str(10000 + int(n))) not in P:
            continue
        try:
            parts = cae_parts(u, P[('2011', b'EVT', n)], int(n))
        except Exception as ex:
            print('part build error', n, ex)
            bad += 5
            continue
        for num, pe, pt in parts:
            good = pe == P[('2011', b'EVP', str(num))] and _no_rt(pt) == _no_rt(P[('2011', b'EVT', str(num))])
            ok += good
            bad += not good
            if not good and bad < 10:
                print('part fail', n, num, pe == P[('2011', b'EVP', str(num))], pt == P[('2011', b'EVT', str(num))])
    print('CAE parts from full entrance: %d identical (EVT rt fields ignored), %d differ' % (ok, bad))


def cmd_convert(argv):
    args = list(argv)
    ref = None
    parts = None
    dup6x = True
    if '--ref' in args:
        i = args.index('--ref')
        ref = args[i + 1:i + 5]
        del args[i:i + 5]
    if '--parts' in args:
        i = args.index('--parts')
        parts = args[i + 1]
        del args[i:i + 2]
    if '--no-dup6x' in args:
        args.remove('--no-dup6x')
        dup6x = False
    evp08, evt08, num, out_evp, out_evt = args
    num = int(num)
    p8, t8 = open(evp08, 'rb').read(), open(evt08, 'rb').read()
    log = []
    e8 = parse_evp(p8, '2008')
    e11 = convert_evp(e8, num, log=log)
    t11 = convert_evt(t8, dup6x=dup6x, log=log)
    if ref:
        r8p, r8t, r11p, r11t = (open(x, 'rb').read() for x in ref)
        r8 = parse_evp(r8p, '2008')
        e11 = transfer_edits(e11, e8, r8, parse_evp(r11p, '2011'), log)
        t11 = transfer_tracks(t11, t8, r8t, r11t, log, e8, r8)
    p11 = build_evp(e11)
    open(out_evp, 'wb').write(p11)
    open(out_evt, 'wb').write(t11)
    print('\n'.join(log))
    print('wrote %s (%d bytes), %s (%d bytes)' % (out_evp, len(p11), out_evt, len(t11)))
    if parts:
        os.makedirs(parts, exist_ok=True)
        for pn, pe, pt in cae_parts(p11, t11, num):
            open(os.path.join(parts, '%d_evp.bin' % pn), 'wb').write(pe)
            open(os.path.join(parts, '%d_evt.bin' % pn), 'wb').write(pt)
            print('part %d: EVP %d bytes, EVT %d bytes' % (pn, len(pe), len(pt)))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
    elif sys.argv[1] == 'validate':
        cmd_validate()
    elif sys.argv[1] == 'selftest':
        cmd_selftest()
    elif sys.argv[1] == 'convert':
        cmd_convert(sys.argv[2:])
    else:
        print(__doc__)
