"""A ported superstar's ratings -> SvR 2011's seven (manifest ratings=), for tools/superstar_port.py.

    ratings, report = map_ratings(game_id, cid, src)

game_id: superstar_port's GAMES ids; src: the converter's game object with `raw_dat` (the CHAR/DAT leaves as
stored). ratings: 7 manifest values (grapple, submission, speed, strikes, hardcore, charisma, durability) - the
game's stored scale, display - 1 (superstar_mods.cpp writes them into the record as they are), 1..99 - or None
(no formula for the game: the base's). report: {'source': text, 'error': mean abs error of the fit, 'note': text}.

Where the ratings are (raw DAT, research 2026-10-13 on every disc): WWE '12 +4..+15 (12 values) - '13 +4..+17
(14 values), 2K14 / 2K15 +36..+49, 2K16 +40..+53, 2K17 +44..+57 (the same 14-value layout; 2K15 on re-rated
people) - SvR 2007 +0..+7 (8 values, a 7..20 scale), all display values (40-100 in steps of 5 from '12 on).
The formulas are fitted on the superstars each game shares with 2011 (24-42 of them, matched by name): per
2011 rating, intercept + coef x field (one or two fields; u16 fields are the height), rounded, 1..99. Their
leave-one-out mean errors (vs ~8-9 for "the roster's mean"): '12 4.6, '13 5.1, 2K14 4.9, 2K15 6.1, 2K16 5.9,
2K17 5.9, SvR 2007 5.9 - close, not exact: the games rate people differently (speed least of all).
SvR 2008 / 2009 (and 2010, svr10_superstar) store 2011's own seven: superstar_port reads them itself.
"""
import struct

ORDER = ('grapple', 'submission', 'speed', 'strikes', 'hardcore', 'charisma', 'durability')

# per rating: (intercept, ((raw offset, coef, 'u8' | 'u16'), ...)); u16 = little-endian at that offset
RATING_LIN = {
    'wwe12': ((-1.0, ((5, 1.0, 'u8'),)), (-6.0, ((8, 1.0, 'u8'),)), (37.71, ((10, 0.544, 'u8'),)),
              (-1.0, ((8, 1.0, 'u8'),)), (11.61, ((5, 0.364, 'u8'), (12, 0.465, 'u8'))),
              (-1.0, ((14, 1.0, 'u8'),)), (30.61, ((4, 0.388, 'u8'), (5, 0.207, 'u8')))),
    'wwe13': ((-39.36, ((5, 0.708, 'u8'), (52, 0.01446, 'u16'))), (-6.0, ((8, 1.0, 'u8'),)),
              (39.98, ((11, 0.492, 'u8'),)), (-1.0, ((8, 1.0, 'u8'),)),
              (8.78, ((5, 0.42, 'u8'), (16, 0.402, 'u8'))), (7.39, ((16, 0.862, 'u8'),)),
              (-14.62, ((5, 0.565, 'u8'), (52, 0.01119, 'u16')))),
    'wwe2k14': ((-39.36, ((37, 0.708, 'u8'), (22, 0.01446, 'u16'))), (-6.0, ((40, 1.0, 'u8'),)),
                (39.98, ((43, 0.492, 'u8'),)), (-1.0, ((40, 1.0, 'u8'),)),
                (8.78, ((37, 0.42, 'u8'), (48, 0.402, 'u8'))), (7.39, ((48, 0.862, 'u8'),)),
                (-14.62, ((37, 0.565, 'u8'), (22, 0.01119, 'u16')))),
    'wwe2k15': ((-1.0, ((37, 1.0, 'u8'),)), (-1.0, ((40, 1.0, 'u8'),)), (4.0, ((43, 1.0, 'u8'),)),
                (-30.29, ((40, 1.04, 'u8'), (41, 0.396, 'u8'))), (-9.39, ((37, 0.692, 'u8'), (48, 0.401, 'u8'))),
                (-1.0, ((48, 1.0, 'u8'),)), (-1.0, ((37, 1.0, 'u8'),))),
    'wwe2k16': ((-1.0, ((41, 1.0, 'u8'),)), (-1.0, ((44, 1.0, 'u8'),)), (4.0, ((47, 1.0, 'u8'),)),
                (-30.29, ((44, 1.04, 'u8'), (45, 0.396, 'u8'))), (-9.39, ((41, 0.692, 'u8'), (52, 0.401, 'u8'))),
                (-1.0, ((52, 1.0, 'u8'),)), (-1.0, ((41, 1.0, 'u8'),))),
    'wwe2k17': ((-1.0, ((45, 1.0, 'u8'),)), (-6.12, ((48, 0.816, 'u8'), (34, 7.675, 'u8'))),
                (39.39, ((51, 0.537, 'u8'),)), (4.0, ((48, 1.0, 'u8'),)),
                (13.99, ((45, 0.419, 'u8'), (56, 0.403, 'u8'))), (15.87, ((56, 0.83, 'u8'),)),
                (-31.53, ((45, 0.484, 'u8'), (30, 0.01726, 'u16')))),
    'svr2007': ((39.74, ((0, 2.453, 'u8'),)), (57.09, ((1, 1.324, 'u8'),)), (19.24, ((2, 3.898, 'u8'),)),
                (30.25, ((3, 3.125, 'u8'),)), (57.92, ((7, 1.357, 'u8'),)), (9.65, ((5, 4.384, 'u8'),)),
                (43.5, ((4, 2.148, 'u8'),))),
}
ERROR = {'wwe12': 4.6, 'wwe13': 5.1, 'wwe2k14': 4.9, 'wwe2k15': 6.1, 'wwe2k16': 5.9, 'wwe2k17': 5.9, 'svr2007': 5.9}
# the record must reach the last field read
NEED = {g: max(o + (2 if k == 'u16' else 1) for _b, terms in lin for o, _a, k in terms) for g, lin in RATING_LIN.items()}


def lin_ratings(r, lin):
    out = []
    for b, terms in lin:
        v = b + sum(a * (r[o] if k == 'u8' else struct.unpack_from('<H', r, o)[0]) for o, a, k in terms)
        out.append(max(1, min(99, int(round(v)))))
    return out


def map_ratings(game_id, cid, src):
    rep = dict(source='', error=None, note='')
    lin = RATING_LIN.get(game_id)
    raw = getattr(src, 'raw_dat', None) or {}
    r = raw.get(cid)
    if not lin:
        rep['note'] = 'no ratings formula for %s' % game_id
        return None, rep
    if r is None or len(r) < NEED[game_id]:
        rep['note'] = 'no DAT record (or a short one) for %d' % cid
        return None, rep
    vals = lin_ratings(bytes(r), lin)
    if len(set(vals)) == 1:  # (a staff / placeholder record: all one value)
        rep['note'] = 'the record has no real ratings (all %d)' % vals[0]
        return None, rep
    rep['source'] = 'fitted on the superstars %s shares with 2011' % game_id
    rep['error'] = ERROR.get(game_id)
    return vals, rep


def report_line(label, ratings, rep):
    if ratings:
        return 'ratings from %s DAT -> %s (fitted on shared superstars: off by ~%s on average)' % (
            label, ','.join(map(str, ratings)), rep['error'])
    return 'ratings: the base\'s (%s)' % rep['note']
