"""SmackDown vs Raw 2010 (Xbox 360) entrance -> SvR 2011 entrance.

Used by tools/entrance_port.py (game id svr10); formats and rules: docs/OTHER_GAMES_ENTRANCES.md
(SvR 2010). 2010 keeps 2011's layouts: the EVP script is already the 2011 format (32-byte
objects, 212-byte slots, the 2011 tail) and the EVT is a full YMKs bank whose ops all have
2011's sizes. Yuke's own 2010 -> 2011 rules, derived from the 184 entrances 2011 ships again
(68 come out byte-identical with these, the rest were re-authored by hand):

- slot track ids + 25000; tracks: ids + 25000, 2011's event-size table, dir rt
  (svr08_entrance.convert_evt on the bank without its 16-byte header);
- title belt prop 2029 -> 5009;
- the global effect object (type 0/0x10000): parent 9999 -> 0 on the 5 segment slots;
- object 6 (an unused extra character): position entries 25..30 filled from 2011's own
  templates (entrance 263 for last index 6.0, another stock one for 7.0), when they are empty;
- object 0 (the entrant): 2010's model when 2011 uses it too, else 16102 (the preview body;
  the entrant replaces it at run time).

Create-An-Entrance parts come from svr08_entrance.cae_parts (2010's own Create*.pac parts follow
another rule and are not converted).
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svr08_entrance as S  # noqa: E402

SEGS = (0, 5, 10, 15, 20)
POS0, POS1 = 12 + 25 * 4, 12 + 31 * 4      # object 6 position entries (params 25..30)
LAST = 12 + 24 * 4                          # its last index (param 24): 6.0 or 7.0


def _last(s):
    return struct.unpack_from('>f', s, LAST)[0]


def obj6_templates(stock_evps):
    """{last index: {segment: 24 bytes}} from 2011's own entrances (263 for 6.0, the first
    with 7.0 otherwise); stock_evps: {number: EVP bytes}"""
    out = {}
    order = [263] + sorted(n for n in stock_evps if n != 263 and n < 800)
    for n in order:
        if n not in stock_evps:
            continue
        try:
            o = S.parse_evp(stock_evps[n], '2011')['objs'][6]
        except Exception:
            continue
        if o['nslots'] != 21 or (o['t0'], o['t1']) != S.TYPE_CHAR:
            continue
        v = _last(o['slots'][0])
        if v in (6.0, 7.0) and v not in out and any(o['slots'][0][POS0:POS1]):
            out[v] = {k: o['slots'][k][POS0:POS1] for k in SEGS}
        if len(out) == 2:
            break
    return out


def convert_evp(e10, number, stock_obj0_models=(), templates=None, log=None):
    log = log if log is not None else []
    e = dict(e10)
    e['num'] = number
    e['f44'] = 0
    objs = []
    for i, o in enumerate(e10['objs']):
        o = dict(o)
        typ = (o['t0'], o['t1'])
        if i == 0 and typ == S.TYPE_CHAR and o['model'] not in stock_obj0_models:
            log.append('object 0: model %d -> %d (preview body)' % (o['model'], S.PREVIEW_CHAR_11))
            o['model'] = S.PREVIEW_CHAR_11
        if typ == S.TYPE_PROP and o['model'] in S.PROP_MAP:
            log.append('object %d: prop %d -> %d' % (i, o['model'], S.PROP_MAP[o['model']]))
            o['model'] = S.PROP_MAP[o['model']]
        slots = []
        for k, s in enumerate(o['slots']):
            s = bytearray(s)
            struct.pack_into('>I', s, 4, S._u32(s, 4) + S.ID_SHIFT)
            if k in SEGS and typ == S.TYPE_GLOBAL and struct.unpack_from('>H', s, S.SLOT - 4)[0] == S.NONE16:
                struct.pack_into('>H', s, S.SLOT - 4, 0)
            if (templates and i == 6 and typ == S.TYPE_CHAR and k in SEGS and not any(s[POS0:POS1])
                    and _last(s) in templates):
                s[POS0:POS1] = templates[_last(s)][k]
            slots.append(bytes(s))
        o['slots'] = slots
        objs.append(o)
    e['objs'] = objs
    if e10['tail']:
        recs = []
        for r in e10['tail']['recs']:
            typ, z, v, items = r[:4]
            items = list(items)
            if typ == 1 and [k for k, _, _ in items] == [2]:
                items += [(1, 0, 0), (1, 0, 0)]
            recs.append((typ, z, v, items) + tuple(r[4:]))
        e['tail'] = dict(num=number, recs=recs)
    e['game'] = '2011'
    return e


def convert_evt(raw10, log=None):
    """2010 EVT (a YMKs bank with its header) -> 2011"""
    if raw10[:4] == b'YMKs':
        raw10 = raw10[16:]
    return S.convert_evt(raw10, dup6x=True, log=log)
