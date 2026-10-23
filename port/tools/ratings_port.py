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

PS2 (tools/ps2_records.py reads the records from the disc): JBI 12 values 0-4, SYM 8 values 0-4, HCTP +0..+4
(strength, submission, speed, technique, endurance; ~5-20), SvR 2004 +0x28..+0x2D (+ charisma), SvR 2006
+0..+7 (5-20) - fitted the same way on 12-22 shared superstars (errors JBI 4.7, SYM 5.9, HCTP 5.1, SvR04 4.9,
SvR06 5.0 vs ~7 for the roster mean; each game's own speed field kept for speed). PS2 2007-2010 hold the X360
games' records byte for byte: 2007 the svr2007 formula, 2008-2010 2011's seven at raw +4..+10 (display values).
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
RATING_LIN.update({
    'ps2_jbi': ((80.84, ((7, 5.374, 'u8'), (10, -4.415, 'u8'))), (57.72, ((1, 4.056, 'u8'), (4, 3.598, 'u8'))),
                (55.59, ((8, -3.653, 'u8'), (10, 9.057, 'u8'))), (59.28, ((10, 7.207, 'u8'),)), (64.05, ((3, 5.841, 'u8'),)),
                (82.17, ((6, -7.115, 'u8'), (10, 8.135, 'u8'))), (62.45, ((4, 4.079, 'u8'), (7, 2.404, 'u8')))),
    'ps2_sym': ((70.46, ((4, 3.61, 'u8'),)), (59.79, ((0, 2.798, 'u8'), (1, 5.173, 'u8'))),
                (63.87, ((4, -3.784, 'u8'), (6, 7.065, 'u8'))), (64.96, ((6, 6.058, 'u8'),)), (72.23, ((7, 3.173, 'u8'),)),
                (76.84, ((6, 3.515, 'u8'),)), (62.6, ((0, 3.563, 'u8'), (3, 2.699, 'u8')))),
    'ps2_hctp': ((43.58, ((0, 2.376, 'u8'),)), (48.04, ((1, 2.039, 'u8'),)), (30.81, ((2, 3.012, 'u8'),)),
                 (38.76, ((1, 2.946, 'u8'),)), (39.67, ((0, 1.511, 'u8'), (3, 1.136, 'u8'))), (56.69, ((4, 1.766, 'u8'),)),
                 (45.54, ((0, 2.133, 'u8'),))),
    'ps2_svr04': ((32.9, ((40, 2.924, 'u8'),)), (45.93, ((41, 2.17, 'u8'),)), (38.47, ((42, 2.64, 'u8'),)),
                  (24.82, ((41, 3.753, 'u8'),)), (26.71, ((40, 1.638, 'u8'), (45, 1.631, 'u8'))), (39.15, ((45, 2.698, 'u8'),)),
                  (39.25, ((40, 2.382, 'u8'),))),
    'ps2_svr06': ((63.32, ((0, 2.003, 'u8'), (2, -1.019, 'u8'))), (64.31, ((1, 1.037, 'u8'),)), (34.59, ((2, 2.903, 'u8'),)),
                  (42.73, ((6, 2.67, 'u8'),)), (35.06, ((0, 1.021, 'u8'), (5, 1.824, 'u8'))),
                  (11.32, ((5, 3.269, 'u8'), (7, 1.278, 'u8'))), (55.61, ((3, -1.25, 'u8'), (4, 2.61, 'u8')))),
})
RATING_LIN['ps2_svr07'] = RATING_LIN['svr2007']   # (the same records)
DIRECT_PS2 = ('ps2_svr08', 'ps2_svr09', 'ps2_svr10', 'ps2_svr11')   # 2011's seven, raw +4..+10, display values
ERROR = {'ps2_jbi': 4.7, 'ps2_sym': 5.9, 'ps2_hctp': 5.1, 'ps2_svr04': 4.9, 'ps2_svr06': 5.0, 'ps2_svr07': 5.9,
         'wwe12': 4.6, 'wwe13': 5.1, 'wwe2k14': 4.9, 'wwe2k15': 6.1, 'wwe2k16': 5.9, 'wwe2k17': 5.9, 'svr2007': 5.9}
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
    if game_id.startswith('ps2_'):
        import ps2_records
        try:
            r = ps2_records.record(src, cid)
        except Exception as e:  # (a disc layout not as researched: the base's, said why)
            rep['note'] = 'the %s character table could not be read (%s)' % (game_id, e)
            return None, rep
        if game_id in DIRECT_PS2 and r is not None and len(r) >= 11:
            vals = [max(1, min(99, x - 1)) for x in r[4:11]]
            if len(set(vals)) == 1:
                rep['note'] = 'the record has no real ratings (all %d)' % vals[0]
                return None, rep
            rep['source'] = '%s stores 2011\'s seven' % game_id
            return vals, rep
    else:
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
    if ratings and rep.get('error') is None:
        return 'ratings from %s -> %s (%s)' % (label, ','.join(map(str, ratings)), rep['source'])
    if ratings:
        return 'ratings from %s DAT -> %s (fitted on shared superstars: off by ~%s on average)' % (
            label, ','.join(map(str, ratings)), rep['error'])
    return 'ratings: the base\'s (%s)' % rep['note']
