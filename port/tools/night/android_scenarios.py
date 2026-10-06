"""Overnight Android scenarios (docs/OVERNIGHT_TESTS.md, "Android") ->
runs/night/android_scenarios.jsonl, for android_night.ps1 (the Fold, one game
at a time). From gen_scenarios.py's suites:
  B  every rule id, CPU vs CPU, to the finish
  C  a superstar sample: every 4th roster superstar + all superstar mods
  D  every arena (and the bundled custom arenas)
  E  the phone's settings: texture quality medium / low, 30 fps, touch on
  G  a soak: back-to-back matches in one run (android_night.ps1 re-routes)
    python android_scenarios.py [--suites B,C,D,E,G]
"""
import argparse
import json
import os

import gen_scenarios as g


def suite_c_sample():
    ids = [i for i, _, note in g.roster() if 'Not selectable' not in note]
    names = {i: n for i, n, _ in g.roster()}
    pick = ids[::4] + g.MOD_IDS
    out = []
    for k, a in enumerate(pick):
        b = pick[(k + 1) % len(pick)]
        out.append(g.match('C%03d_%03d' % (a, b), 'C', '%s vs %s' % (names.get(a, 'mod %d' % a), names.get(b, 'mod %d' % b)),
                           [a, b], timeout=420, shots=(40, 120, 240)))
    return out


def suite_e_phone():
    v = [('tex_high', ['--native_texture_quality=high']), ('tex_medium', ['--native_texture_quality=medium']),
         ('tex_low', ['--native_texture_quality=low']), ('fps30', ['--frame_rate=30']),
         ('fps60', ['--frame_rate=60']), ('touch_on', ['--touch_controls=true'])]
    return [g.match('E_' + n, 'E', 'phone settings ' + n, ['JOHN CENA', 'RANDY ORTON'], arena=17, args=a,
                    timeout=540, shots=(60, 240, 480)) for n, a in v]


def suite_g():
    s = g.match('G_soak', 'G', 'soak: back-to-back matches', ['JOHN CENA', 'RANDY ORTON'], arena=17,
                timeout=7200, shots=(600, 1800, 3600, 5400))
    s['soak'] = True
    return [s]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--suites', default='B,C,D,E,G')
    ap.add_argument('--out', default=os.path.join(g.PORT, 'runs', 'night', 'android_scenarios.jsonl'))
    a = ap.parse_args()
    gens = {'B': g.suite_b, 'C': suite_c_sample, 'D': g.suite_d, 'E': suite_e_phone, 'G': suite_g}
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
    print(a.out, counts)


if __name__ == '__main__':
    main()
