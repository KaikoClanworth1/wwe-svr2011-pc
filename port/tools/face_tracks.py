"""Face tracks for converted entrances (report a1b6b1, docs/OTHER_GAMES_ENTRANCES.md "Faces").

2011 poses the entrant's face with a third track beside the body (y0) and the fingers (y2 right, y3 left):
y1, one face-rig pose a record (288 bytes, one a frame, in the older entrances; 393 bytes, key-reduced, in
the newer ones - not decoded here). An entrance without it leaves the face as the last motion set it: in
Create An Entrance a Diva's smile stays on through a converted entrance played after hers (report a1b6b1).

Games that keep the face elsewhere get one from 2011's own entrances: WWE '12 - 2K17 have it as hashed face
bones inside the body record (a rig 2011 has no map for), HCTP / SvR 2004 / Shut Your Mouth / Just Bring It
have none (face cues by event only). Each converted segment of the entrant gets the face of
  - the same superstar's 2011 entrance (prefer: the caller's number when 2011 has it under that name), else
  - 522 "General Entrance" (2011's default entrance for created superstars),
from the segment with the same slot (x), else the one closest in length; its poses only (the stock track's
own cues - op 193 sound / voice, 100-103, 162, 171, ... - belong to that entrance's timing and are dropped),
trimmed or looped to the segment's length, the first record with the event block every stock y1-y3 track
starts with (hand_tracks.FIRST_EVENTS).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ymks  # noqa: E402

DEFAULT_DONOR = 522                                       # (2011's "General Entrance")
FIRST_EVENTS = bytes.fromhex('4005000000008900010000')   # (= hand_tracks.FIRST_EVENTS)
FACE_Y = 1


_banks = {}


def stock_bank(game_pac, number=DEFAULT_DONOR):
    """the EVT bank of a 2011 entrance from the game's pac folder (pac/evt/Nyujyo*.pac), None if not found"""
    key = (game_pac, number)
    if key not in _banks:
        _banks[key] = None
        if game_pac and os.path.isdir(os.path.join(game_pac, 'evt')):
            import entrance_port   # (late: entrance_port imports the converters that use this module)
            for f in sorted(os.listdir(os.path.join(game_pac, 'evt'))):
                if not (f.lower().startswith('nyujyo') and f.lower().endswith('.pac')):
                    continue
                try:
                    ents = entrance_port.pac_entries(open(os.path.join(game_pac, 'evt', f), 'rb').read(),
                                                     lambda g, n: 1 if g.startswith('EVT') and n == str(number) else 0)
                except (ValueError, OSError):
                    continue
                for (g, n), v in ents.items():
                    _banks[key] = ymks.read_bank(v)
                if _banks[key] is not None:
                    break
    return _banks[key]


def add_to_bank(bank, obj, segments, game_pac, rep, rt):
    """the stock default face (y1) on each segment {x: nframes} of object obj in a YMKs bank being built;
    rep: dict with lists notes / warnings -> the number of tracks added"""
    made = face_entries(segments, obj, [(DEFAULT_DONOR, stock_bank(game_pac))] if stock_bank(game_pac) else [], rep)
    for z in made:
        ymks.insert_motion(bank, 25000 + obj, z['x'], FACE_Y, z['data'], z['frames'], rt)
    return len(made)


def donor_faces(bank, obj=0):
    """{x: (data, frames, rt)} of a 2011 entrance bank's face tracks for object obj (y1)"""
    if bank is None:
        return {}
    return {e['x']: (e['data'], e['frames'], e['rt']) for e in bank.entries
            if e['id'] == 25000 + obj and e['y'] == FACE_Y and e['data']}


def fit(data, nframes):
    """a stock face track's poses -> nframes frames of them (trimmed, or looped from its start), events dropped
    but frame 0's standard block; None when the track has no pose to start from"""
    st = ymks.parse_stream(data)
    poses = [(f, ymks.split_record(r)[1]) for f, r in st.records()]
    poses = [(f, p) for f, p in poses if p]
    if not poses or poses[0][0] != 0:
        return None
    length = max(st.nframes(), poses[-1][0] + 1)
    out = []
    for start in range(0, max(1, nframes), length):
        for f, p in poses:
            if start + f >= nframes:
                break
            out.append((start + f, ymks.join_record(FIRST_EVENTS, p) if start + f == 0 else p))
    return ymks.build_stream(ymks.make_stream(out, nframes))


def pick(faces, x, nframes):
    """the face to use for slot x: the same slot, else the one closest in length"""
    if x in faces:
        return faces[x]
    return min(faces.values(), key=lambda v: (abs(v[1] - nframes), -v[1])) if faces else None


def face_entries(segments, obj, banks, rep, label=''):
    """segments: {x: nframes} of the converted entrant (object obj); banks: [(number, 2011 bank)] in order of
    preference (the same superstar's, then DEFAULT_DONOR's) -> YMKs entries for the y1 face tracks"""
    out, used = [], {}
    for x, nf in sorted(segments.items()):
        for n, bank in banks:
            # (a team member: the donor's object of the same index when it has a face, else its entrant's)
            got = pick(donor_faces(bank, obj) or donor_faces(bank, 0), x, nf)
            data = fit(got[0], nf) if got else None
            if data:
                out.append(dict(y=FACE_Y, x=x, id=25000 + obj, frames=nf, rt=got[2], data=data, share=False,
                                tail_off=None))
                used[x] = n
                break
    if used:
        by = sorted(set(used.values()))
        rep['notes'].append('%sface: 2011 entrance %s\'s face tracks on %d segment(s) (the source\'s face is not '
                            'converted)' % (label, ' / '.join(str(n) for n in by), len(used)))
    elif segments:
        rep['warnings'].append('%sno face tracks (no 2011 pac folder to take them from): the face keeps what played '
                               'before' % label)
    return out
