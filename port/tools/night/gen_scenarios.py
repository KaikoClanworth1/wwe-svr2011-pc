"""Overnight test scenarios (docs/OVERNIGHT_TESTS.md) -> runs/night/scenarios.jsonl.

    python gen_scenarios.py [--suites B,C,D,E] [--out <file>]

One JSON line per scenario: id, suite, name, env (SVR2011_* test aids), args
(extra --cvar=value), timeout (s), pass (regex the log must show after the
"test match:" line), shots (seconds into the run for screenshots).
The runner (night.ps1) runs them, 4 at a time, and writes results.jsonl.
"""
import argparse
import json
import os
import re

PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
TOP = os.path.dirname(PORT)

# A finished match: the ending's replay steps (replays.cpp "match end: step N").
MATCH_DONE = r'match end: step 3'

# The superstar mods' ids (superstar_mods.cpp assigns them in this order with
# the 22 bundled mods on; the log's "superstar mods: X as id N").
MOD_IDS = [111, 114, 121, 127, 128, 129, 130, 136, 141, 148, 149, 151, 152, 154, 155, 157, 162, 163,
           167, 168, 172, 173]


def roster():
    """(id, name, note) from the launcher's roster table."""
    out = []
    text = open(os.path.join(PORT, 'launcher', 'roster_names.inc'), encoding='utf-8').read()
    for m in re.finditer(r'\{\s*(\d+),\s*L"([^"]*)",\s*L"([^"]*)"\s*\}', text):
        out.append((int(m.group(1)), m.group(2), m.group(3)))
    return out


def match(id_, suite, name, people, rule=None, arena=None, timeout=480, args=(), env=None, shots=(45, 150, 300)):
    tm = 'people=' + ','.join(str(p) for p in people) + ' cpu=all'
    if arena is not None:
        tm += ' arena=%d' % arena
    e = {'SVR2011_ROUTE': 'match', 'SVR2011_TEST_MATCH': tm}
    if rule is not None:
        e['SVR2011_TEST_RULE'] = '%02X' % rule
    if env:
        e.update(env)
    return {'id': id_, 'suite': suite, 'name': name, 'env': e, 'args': list(args), 'timeout': timeout,
            'pass': MATCH_DONE, 'shots': [s for s in shots if s < timeout]}


def suite_b():
    """Every rule id, CPU vs CPU, to the finish."""
    out = []
    long_rules = {0x14, 0x15, 0x16, 0x17, 0x26, 0x52, 0x58}  # rumbles, Championship Scramble, gauntlets: longer
    for r in range(0x00, 0x80):
        t = 1200 if r in long_rules else 540
        out.append(match('B%02X' % r, 'B', 'rule %02X' % r, ['JOHN CENA', 'RANDY ORTON'], rule=r, timeout=t,
                         shots=(60, 240, 480, 900)))
    return out


def suite_c():
    """Every superstar (roster + mods), twice (once per side), entrances on."""
    ids = [i for i, _, note in roster() if 'Not selectable' not in note] + MOD_IDS
    names = {i: n for i, n, _ in roster()}
    out = []
    n = len(ids)
    for k in range(n):
        a, b = ids[k], ids[(k + 1) % n]
        out.append(match('C%03d_%03d' % (a, b), 'C', '%s vs %s' % (names.get(a, 'mod %d' % a), names.get(b, 'mod %d' % b)),
                         [a, b], timeout=420, shots=(40, 120, 240)))
    return out


def suite_d():
    """Every arena slot 0..59 (empty slots show up as errors) and the bundled custom arenas."""
    out = []
    for a in range(0, 60):
        out.append(match('D%02d' % a, 'D', 'arena %d' % a, ['JOHN CENA', 'RANDY ORTON'], arena=a, timeout=360,
                         shots=(40, 120, 240)))
    # The installed custom arenas, each on its base arena's slot (as the select
    # screen places it): SVR2011_TEST_ARENA_REDIRECT=<slot>=<arena.pac>.
    slots = {'raw_is_war': 1, 'smackdown_1999': 0, 'king_of_the_ring_1998': 6, 'royal_rumble_1998': 9}
    arenas = os.path.join(TOP, 'Game Files', 'Mods', 'Arenas')
    for folder in sorted(os.listdir(arenas)) if os.path.isdir(arenas) else []:
        slot = next((s for k, s in slots.items() if folder.startswith(k)), None)
        pac = os.path.join('Mods', 'Arenas', folder, 'arena.pac')
        if slot is None or not os.path.exists(os.path.join(TOP, 'Game Files', pac)):
            continue
        out.append(match('Dmod_' + folder, 'D', 'arena mod ' + folder, ['JOHN CENA', 'RANDY ORTON'], arena=slot,
                         timeout=360, env={'SVR2011_TEST_ARENA_REDIRECT': '%d=%s' % (slot, pac)},
                         shots=(40, 120, 240)))
    return out


def suite_e():
    """Settings variants on one fixed match."""
    v = [
        ('d3d12', []), ('vulkan', ['--gpu_backend=vulkan']),
        ('vulkan_d32', ['--gpu_backend=vulkan'], {'SVR2011_NATIVE_DEPTH_D32': '1'}),
        ('fps30', ['--frame_rate=30']), ('fps30_half', ['--frame_rate=30'], {'SVR2011_TEST_HALF_30': '1'}),
        ('tex_medium', ['--native_texture_quality=medium']), ('tex_low', ['--native_texture_quality=low']),
        ('aa_off', ['--native_aa=1']), ('aa_4x', ['--native_aa=4']),
        ('scale_3x', ['--native_max_scale=3', '--resolution_scale=3']),
        ('mixed_off', ['--mixed_gender_matches=false']), ('unlock_off', ['--unlock_everything=false']),
        ('touch_on', ['--touch_controls=true']), ('full_speed_off', ['--full_speed=false']),
    ]
    out = []
    for item in v:
        name, args = item[0], item[1]
        env = item[2] if len(item) > 2 else None
        out.append(match('E_' + name, 'E', 'settings ' + name, ['JOHN CENA', 'RANDY ORTON'], arena=17, args=args,
                         env=env, timeout=540, shots=(60, 240, 480)))
    return out


def suite_m():
    """The port's row-based modes (SVR2011_TEST_MODE, match_types.cpp; needs fa35fd9 or later)."""
    modes = [('three_stages', ['JOHN CENA', 'RANDY ORTON'], 'three stages: on', 900),
             ('elimination_tt', ['JOHN CENA', 'RANDY ORTON', 'EDGE'], r'ELIMINATION \(rule 0D\)', 720),
             ('elimination_f4w', ['JOHN CENA', 'RANDY ORTON', 'EDGE', 'BATISTA'], r'ELIMINATION \(rule 0E\)', 900),
             ('weapons', ['JOHN CENA', 'RANDY ORTON'], r'WEAPONS EVERYWHERE \(rule 4D\)', 600),
             ('slobber', ['JOHN CENA', 'RANDY ORTON'], 'SLOBBER KNOCKER', 1200),
             ('lumberjack', ['JOHN CENA', 'RANDY ORTON'], 'lumberjacks: ', 720)]
    out = []
    for name, people, needs, t in modes:
        for arena in (17, 1):
            m = match('M_%s_%d' % (name, arena), 'M', 'mode %s, arena %d' % (name, arena), people, arena=arena,
                      timeout=t, env={'SVR2011_TEST_MODE': name}, shots=(60, 240, 480, 900))
            m['needs'] = needs  # the mode's own log line must show, or it didn't run
            out.append(m)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--suites', default='B,C,D,E')
    ap.add_argument('--out', default=os.path.join(PORT, 'runs', 'night', 'scenarios.jsonl'))
    a = ap.parse_args()
    gens = {'B': suite_b, 'C': suite_c, 'D': suite_d, 'E': suite_e, 'M': suite_m}
    rows = []
    for s in a.suites.split(','):
        rows += gens[s.strip()]()
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        for r in rows:
            f.write(json.dumps(r) + '\n')
    counts = {}
    for r in rows:
        counts[r['suite']] = counts.get(r['suite'], 0) + 1
    print(len(rows), 'scenarios', counts, '->', a.out)


if __name__ == '__main__':
    main()
