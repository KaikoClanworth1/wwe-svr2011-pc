"""A ported superstar's abilities -> SvR 2011 ability ids (manifest abilities=), for tools/superstar_port.py.

    ids, report = map_abilities(game_id, cid, src, ratings=None, style=None)

game_id: superstar_port's GAMES ids (svr2007 .. wwe2k17). src: the converter's game object (its DAT records:
`raw_dat` - the PACH leaves as stored - or the 2007-2009 objects' `g.dat`, the leaf after its 4-byte header).
ratings: the mod's 2011 ratings (7 values) when known - for the guess. ids: up to 5 ids (the stock maximum),
[] = keep the base's. report: {'mapped': [...], 'guessed': [...], 'dropped': [...], 'notes': [...]} of text.

2011's abilities (CHAR/DAT +230 after the header, u8 ids ending at 0; name string 0x9EA8 + id) - those the
game uses: 1 Dirty Pin, 7 Move Thief, 9 Hammer Throw, 11 Resiliency, 12 Durability, 14 Kip-Up, 19 Outside Dives,
20 Springboard Dives, 22 Leverage Pin, 23 Fired Up, 24 Ring Escape (the other ids still have names, but no
superstar has them and some descriptions are placeholders: not used).

The other games (research 2026-10-13, every disc read): SvR 2009 .. WWE 2K17 share one numbering, 1 Dirty Pin
.. 27 Strong Strike; only two meanings moved - 22 Possum Pin (2009 / 2010) became 2011's Leverage Pin (a
pin reversal either way), 14 Kip-Up became Comeback from WWE '12 on (a rise from heavy damage either way, but
the holders differ: a close match, reported as such). Where the list is (raw leaf offset, slots):
  2009 +232 (6), 2010 +234 (6), WWE '12 +260 (8, 290-byte DAT), '13 +274 (8, 324 B), 2K14 / 2K15 +56 (8, 208 B),
  2K16 +60 (8, 216 B), 2K17 +64 (8, 220 B).
SvR 2008 has no list: two fighting styles (raw misc.pac CHAR/DAT +227 first, +228 second; 0-7, 8 = none),
mapped by what the 2008 style help text grants (High-Flyer: dives, springboards, possum pin; Powerhouse: the
strong whip; Showman: steal the finisher ...) - reported as guessed. SvR 2007 has none: guessed from the ratings.
"""

USED_2011 = (1, 7, 9, 11, 12, 14, 19, 20, 22, 23, 24)
NAMES_2011 = {1: 'Dirty Pin', 7: 'Move Thief', 9: 'Hammer Throw', 11: 'Resiliency', 12: 'Durability', 14: 'Kip-Up',
              19: 'Outside Dives', 20: 'Springboard Dives', 22: 'Leverage Pin', 23: 'Fired Up', 24: 'Ring Escape'}
# the other games' names (2009 / 2010 first; '12+ where it differs)
SOURCE_NAMES = {1: 'Dirty Pin', 2: 'Evasive Dodge', 3: 'K.O.', 4: 'Lock Pick', 5: 'Table Match', 6: 'Referee Shield',
                7: 'Move Thief', 8: 'Taunt Thief', 9: 'Hammer Throw', 10: 'Object Specialist', 11: 'Resiliency',
                12: 'Durability', 13: 'Hardcore Resurrection', 14: 'Kip-Up', 15: 'Fan Favorite', 17: 'Ladder Match',
                18: 'Cage Match', 19: 'Outside Dives', 20: 'Springboard Dives', 21: 'Submission', 22: 'Possum Pin',
                23: 'Fired Up', 24: 'Ring Escape', 25: 'Exploder Turnbuckle Attack', 26: 'Pull-Back Attacks',
                27: 'Strong Strike'}
LATER_NAMES = {14: 'Comeback', 22: 'Leverage Pin'}  # WWE '12 on

# raw DAT leaf offset of the id list, slots
SLOTS = {'svr2009': (232, 6), 'svr2010': (234, 6), 'wwe12': (260, 8), 'wwe13': (274, 8), 'wwe2k14': (56, 8),
         'wwe2k15': (56, 8), 'wwe2k16': (60, 8), 'wwe2k17': (64, 8)}
LATER = ('wwe12', 'wwe13', 'wwe2k14', 'wwe2k15', 'wwe2k16', 'wwe2k17')
CAP = 5

# SvR 2008 fighting style -> 2011 abilities (from the 2008 style help strings 18185-18252)
STYLES_2008 = {0: ('Powerhouse', [9]), 1: ('Technical', []), 2: ('High-Flyer', [19, 20, 22]), 3: ('Brawler', []),
               4: ('Submission', [11]), 5: ('Dirty', [1]), 6: ('Showman', [7]), 7: ('Hardcore', [19])}


def _raw(src, cid):
    """the superstar's DAT leaf as stored (with its 4-byte header), or None"""
    raw = getattr(src, 'raw_dat', None)
    if raw and cid in raw:
        return bytes(raw[cid])
    g = getattr(src, 'g', None)
    dat = getattr(g, 'dat', None) if g is not None else None
    if dat and cid in dat:
        return bytes(4) + bytes(dat[cid])  # (the 2009 / 2010 readers drop the header: put 4 bytes back)
    return None


def guess_from_ratings(ratings):
    """2011 ratings (grapple, submission, speed, strikes, hardcore, charisma, durability) -> a likely set,
    by who has what among 2011's own superstars (fast: dives; heavy grapplers: hammer throw ...)"""
    if not ratings or len(ratings) < 7:
        return []
    grapple, sub, speed, strikes, hardcore, charisma, durability = [int(x) for x in ratings[:7]]
    out = []
    if speed >= 80:
        out.append(19)
    if speed >= 86:
        out.append(20)
    if grapple >= 82 and speed < 72:
        out.append(9)
    if durability >= 84:
        out.append(11)
    if charisma >= 88:
        out.append(23)
    if hardcore >= 85 and 19 not in out:
        out.append(19)
    return out


def map_abilities(game_id, cid, src, ratings=None, style=None):
    """(PS2 game ids: ps2_svr08 / 09 / 10 hold the X360 games' records - read as those; the older PS2 games have
    no per-superstar abilities: guessed from the ratings)"""
    rep = dict(mapped=[], guessed=[], dropped=[], notes=[])
    ids = []
    if game_id.startswith('ps2_'):
        import ps2_records
        twin = ps2_records.X360_TWIN.get(game_id)
        try:
            rec = ps2_records.record(src, cid) if twin in ('svr2008', 'svr2009', 'svr2010') else None
        except Exception as e:
            rec = None
            rep['notes'].append('the %s character table could not be read (%s)' % (game_id, e))
        if rec is not None:
            class _One:   # (the record as raw_dat, for the X360 path below)
                raw_dat = {cid: rec}
            src, game_id = _One(), twin
        else:
            rep['notes'].append('%s keeps no abilities' % game_id)
            game_id = 'none'


    def add(a, kind, text):
        if a in USED_2011 and a not in ids and len(ids) < CAP:
            ids.append(a)
            rep[kind].append(text)

    rec = _raw(src, cid)
    if game_id == 'svr2008':
        styles = [s for s in (rec[227], rec[228]) if s < 8] if rec is not None and len(rec) > 228 else []
        for s in styles:
            name, got = STYLES_2008[s]
            for a in got:
                add(a, 'guessed', '%s (2008 fighting style %s)' % (NAMES_2011[a], name))
            if not got:
                rep['notes'].append('2008 fighting style %s has no 2011 ability' % name)
        if not styles:
            rep['notes'].append('no 2008 fighting style')
    elif game_id in SLOTS and rec is not None:
        off, n = SLOTS[game_id]
        src_ids = []
        for x in rec[off:off + n]:
            if not x:
                break
            src_ids.append(x)
        names = dict(SOURCE_NAMES)
        if game_id in LATER:
            names.update(LATER_NAMES)
        for x in src_ids:
            sn = names.get(x, 'ability %d' % x)
            if x not in USED_2011:
                rep['dropped'].append('%s (not in 2011)' % sn)
                continue
            if game_id in LATER and x == 14:
                add(14, 'guessed', 'Comeback -> Kip-Up (close: both a rise from heavy damage)')
            elif x == 22 and game_id in ('svr2009', 'svr2010'):
                add(22, 'mapped', 'Possum Pin -> Leverage Pin')
            else:
                add(x, 'mapped', NAMES_2011[x])
        if not src_ids:
            rep['notes'].append('the superstar has no abilities in %s' % game_id)
    elif game_id in SLOTS or game_id == 'svr2007':
        rep['notes'].append('%s: no abilities to read' % game_id)
    if not ids:  # nothing from the source: by the ratings
        for a in guess_from_ratings(ratings):
            add(a, 'guessed', '%s (from the ratings)' % NAMES_2011[a])
    return ids, rep


def report_lines(source_label, ids, rep):
    """notes for the converter's report: what came from where"""
    lines = []
    if rep['mapped']:
        lines.append('abilities from %s: %s' % (source_label, ', '.join(rep['mapped'])))
    if rep['guessed']:
        lines.append('abilities guessed: %s' % ', '.join(rep['guessed']))
    if rep['dropped']:
        lines.append('abilities not in 2011: %s' % ', '.join(rep['dropped']))
    lines += ['abilities: ' + n for n in rep['notes']]
    if not ids:
        lines.append('abilities: the base\'s')
    return lines
