"""A ported superstar's own moveset in SvR 2011 terms (superstar_port's moves step).

    port_moveset(ctx) -> {'moves_txt': [lines], 'rows': [...], 'pack': folder or None}

For each slot of the source game's profile that tools/moveset_maps.json maps to a 2011 slot:
  - fixed 2011 slots (the same id for the whole roster, pro_slots.json) stay as they are;
  - the source move translated by name (MOVS/WAZE) to a 2011 move whose motion is in the slot's
    category (pro_slots.json "bank") -> that move;
  - else, when the source has its motion and a porter exists (moves_port.py: WWE '12 / '13 / 2K14, SvR
    2007-2009 - their tracks copied onto 2011's own record of the move),
    the move is ported (the template method) into a move pack inside the mod, on a free 2011
    record - one per source move, picked by the superstar and move so two mods rarely meet;
  - else the base's move stays.
moves.txt holds the slots that differ from the base's profile (0xOFF=id). Every row is reported.
A 2011 move is used when it has a motion (some records are cut moves) and 2011's own roster has it in that
slot or in the same menu group (pro_slots.json "menu") - a strike doesn't land in a grapple slot.
Taunts (unless port_taunts is False - superstar_port --no-taunts) are ported on a taunt template (the base's taunt in that slot, else another 2011 taunt on YMBs
banks), on a free record in the template's thousand: moves_port's YMBs path. Submissions on YMBs banks
keep the base's.
"""
import collections
import json
import os
import shutil
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
N11 = 0x1C0 // 2


def _norm(s):
    import moves_port as MP
    return MP.norm(s)


class Ctx:
    """gid, cid, base, g11 (superstar_port 2011 game), pro (source profile bytes), waze (source {id: name}),
    mgame (moves_port game for porting, or None), pac11, svrmod, work, mod, rep, port (bool), max_port"""

    def __init__(self, **kw):
        self.__dict__.update(kw)


_G11M = {}


def g11_moves(pac11, work):
    import moves_port as MP
    if pac11 not in _G11M:
        _G11M[pac11] = MP.Game11(pac11, os.path.join(work, '.mv11'))
    return _G11M[pac11]


def port_moveset(c):
    import moves_port as MP
    maps = json.load(open(os.path.join(HERE, 'moveset_maps.json')))
    if c.gid not in maps:
        return None
    smap = {int(s, 16): int(t, 16) for s, (t, sc) in maps[c.gid]['map'].items()}
    slots = {int(s['off']): s for s in json.load(open(os.path.join(HERE, 'pro_slots.json')))['slots']}
    w11 = c.g11.waze()
    by11 = collections.defaultdict(set)
    for i, (bits, nm) in w11.items():
        by11[_norm(nm)].add(i)
    g11m = g11_moves(c.pac11, c.work)
    moved = g11m.moved   # (2011 moves with a motion; some records are cut moves without one)
    # where 2011's own roster uses each move: by slot, and by the slot's menu group (pro_slots "menu")
    menu_of = {off: (sl.get('menu') or '') for off, sl in slots.items()}
    in_slot, in_menu = collections.defaultdict(set), collections.defaultdict(set)
    for r in c.g11.pro.values():
        if len(r) < 2 * N11:
            continue
        for k, x in enumerate(struct.unpack_from('<%dH' % N11, r, 0)):
            in_slot[x].add(2 * k)
            in_menu[x].add(menu_of.get(2 * k, ''))
    base = list(struct.unpack_from('<%dH' % N11, c.g11.pro[c.base], 0))
    area = list(base)
    rows = []
    to_port = collections.OrderedDict()   # source id -> [2011 offsets]
    src_index = c.mgame.index() if c.mgame is not None else {}
    templ = {}   # source id -> the 2011 taunt template (YMBs banks)

    def ymbs_taunt(mid):
        return mid in moved and any(kd == 'YMBs' for p, kd, ks in g11m.banks_of(mid))
    any_taunt = None
    for so, to in sorted(smap.items()):
        if so + 2 > len(c.pro) or to // 2 >= N11:
            continue
        v = struct.unpack_from('<H', c.pro, so)[0]
        sl = slots.get(to, {})
        nm = c.waze.get(v, '')
        if sl.get('fixed'):
            continue
        bank = (sl.get('bank') or '').strip()
        if getattr(c, 'same_ids', False) and v in w11:   # (SvR 2007-2009: 2011's ids)
            cand = [v]
        else:
            cand = sorted(by11.get(_norm(nm), set())) if nm else []
        fit = [x for x in cand if x in moved and (to in in_slot[x] or menu_of.get(to, '') in in_menu[x])]
        if fit:
            area[to // 2] = fit[0]
            rows.append((so, to, v, nm, 'same move in 2011', fit[0]))
        elif menu_of.get(to) == 'Taunts' and v in src_index and not getattr(c, 'port_taunts', True):
            rows.append((so, to, v, nm, 'base kept (a taunt: --no-taunts)',
                         base[to // 2]))
        elif v in src_index and c.port and c.mgame is not None:
            if menu_of.get(to) == 'Taunts' and v not in templ:   # (a taunt plays from YMBs banks: a taunt template)
                t = base[to // 2] if ymbs_taunt(base[to // 2]) else None
                if t is None:
                    if any_taunt is None:
                        any_taunt = next((x for x in sorted(in_menu) if 'Taunts' in in_menu[x] and ymbs_taunt(x)), 0)
                    t = any_taunt or None
                if t is None:
                    rows.append((so, to, v, nm, 'base kept (no 2011 taunt template)', base[to // 2]))
                    continue
                templ[v] = t
            to_port.setdefault(v, []).append(to)
        else:
            rows.append((so, to, v, nm, 'base kept (%s)' % (
                ('no motion in 2011' if not any(x in moved for x in cand) else 'used elsewhere in 2011') if cand else
                                                             'no porter for this game yet' if c.mgame is None else
                                                             'no motion in the source' if v not in src_index else
                                                             'porting off'), base[to // 2]))
    pack = None
    if to_port:
        free = [i for i in sorted(w11) if i not in g11m.moved and i > 1000]
        used = set()
        pdir = os.path.join(c.work, 'moveset_port')
        if os.path.exists(pdir):
            shutil.rmtree(pdir)
        lines, motions = [], []
        for k, (v, tos) in enumerate(to_port.items()):
            nm = c.waze.get(v, 'MOVE %d' % v)
            if c.max_port is not None and k >= c.max_port:
                for to in tos:
                    rows.append((None, to, v, nm, 'base kept (over the %d ported moves)' % c.max_port, base[to // 2]))
                continue
            h = zlib.crc32(('%s/%d/%d' % (c.gid, c.cid, v)).encode())
            new_id = None
            if getattr(c, 'same_ids', False) and v in w11 and v not in g11m.moved and v > 1000 and v not in used:
                new_id = v   # (SvR 2007-2009: 2011 keeps the move's own record, without a motion)
            pool = free
            if v in templ:   # (a taunt: a record in the template's thousand - 9000-9999; elsewhere the match didn't find it)
                pool = [i for i in free if i // 1000 == templ[v] // 1000] or free
            for d in range(len(pool) if new_id is None else 0):
                cand = pool[(h + d) % len(pool)]
                if cand not in used:
                    new_id = cand
                    break
            if new_id is None:
                for to in tos:
                    rows.append((None, to, v, nm, 'base kept (no free 2011 record)', base[to // 2]))
                continue
            used.add(new_id)
            r = {'notes': [], 'warnings': [], 'dropped': []}
            a = dict(game=c.gid, item=str(v), out=os.path.join(pdir, str(v)), pac11=c.pac11, svrmod=c.svrmod,
                     id=str(new_id), _g=c.mgame, _g11=g11m)
            if v in templ:
                a['template'] = str(templ[v])
            try:
                MP.convert(a, r)
            except Exception as e:   # noqa: BLE001
                for to in tos:
                    rows.append((None, to, v, nm, 'base kept (port failed: %s)' % str(e)[:80], base[to // 2]))
                continue
            for to in tos:
                area[to // 2] = new_id
                rows.append((None, to, v, nm, 'ported (template %s)' % r.get('template'), new_id))
            lines += open(os.path.join(r['folder'], 'pack.txt')).read().splitlines()
            motions.append(os.path.join(r['folder'], 'motions'))
        if lines:
            pack = os.path.join(c.work, 'moveset_pack')
            if os.path.exists(pack):
                shutil.rmtree(pack)
            os.makedirs(os.path.join(pack, 'motions'))
            for m in motions:
                for f in os.listdir(m):
                    shutil.copyfile(os.path.join(m, f), os.path.join(pack, 'motions', f))
            open(os.path.join(pack, 'pack.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    moves_txt = ['0x%03X=%d' % (k * 2, v) for k, v in enumerate(area) if v != base[k]]
    return {'moves_txt': moves_txt, 'rows': rows, 'pack': pack}


def merge_pack(pack, mod):
    """a move pack into the mod's moves/ (the entrance pack may be there already: lines appended)"""
    dst = os.path.join(mod, 'moves')
    os.makedirs(os.path.join(dst, 'motions'), exist_ok=True)
    for f in os.listdir(os.path.join(pack, 'motions')):
        shutil.copyfile(os.path.join(pack, 'motions', f), os.path.join(dst, 'motions', f))
    old = open(os.path.join(dst, 'pack.txt')).read() if os.path.exists(os.path.join(dst, 'pack.txt')) else ''
    new = open(os.path.join(pack, 'pack.txt')).read()
    open(os.path.join(dst, 'pack.txt'), 'w', newline='\n').write(old + ('' if not old or old.endswith('\n') else '\n') + new)


def summary(res):
    c = collections.Counter(r[4].split(' (')[0] for r in res['rows'])
    return ', '.join('%s %d' % kv for kv in sorted(c.items()))
