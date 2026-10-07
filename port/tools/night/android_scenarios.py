"""Overnight Android scenarios (docs/OVERNIGHT_TESTS.md, "Android") ->
runs/night/android_scenarios.jsonl, for android_night.ps1 (the Fold, one game
at a time). From gen_scenarios.py's suites:
  B  every rule id, CPU vs CPU, to the finish
  C  a superstar sample: every 4th roster superstar + all superstar mods
  D  every arena (and the bundled custom arenas)
  E  the phone's settings: texture quality medium / low, 30 fps, touch on
  G  a soak: back-to-back matches in one run (android_night.ps1 re-routes)
  T  the port's row-based modes (gen_scenarios' suite M: SVR2011_TEST_MODE, a 'needs' log line)
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


def suite_b_phone():
    """gen_scenarios' B, with Championship Scramble (0x26: a 5:00 timed match + entrances) given longer."""
    out = g.suite_b()
    for r in out:
        if r['id'] == 'B26':
            r['timeout'] = 1500
            r['shots'] = [60, 240, 480, 900, 1200]
    return out


def suite_m():
    """MY MUSIC (jukebox.cpp): three 15 s test songs (runs/night/android_music, pushed to
    test_run/night_music, SVR2011_MUSIC), the game's songs off. M_handover: the menus hand
    over from song to song (3 starts); M_match: a match stops the song (its end, and the
    entrance music)."""
    off = ','.join(str(n) for n in range(1, 20))
    songs = 'Pena_Theme.mp3|Test_A.mp3|Test_C.mp3'.replace('Pena', 'Peña')
    args = ['--jukebox_off=' + off, '--jukebox_my_music=' + songs, '--audio_mute=false']
    env = {'SVR2011_MUSIC': 'night_music'}
    hand = {'id': 'M_handover', 'suite': 'M', 'name': 'MY MUSIC: songs hand over in the menus',
            'env': dict(env, SVR2011_ROUTE='main'), 'args': args, 'timeout': 150,
            'pass': r'now playing .* \(my music\)', 'need': 3, 'shots': [30]}
    m = g.match('M_match', 'M', 'MY MUSIC: a match stops the song', ['JOHN CENA', 'RANDY ORTON'], arena=17,
                args=args, env=env, timeout=540, shots=(60, 240))
    m['expect'] = r'my music stopped|game music unmuted'
    return [hand, m]


def suite_g():
    s = g.match('G_soak', 'G', 'soak: back-to-back matches', ['JOHN CENA', 'RANDY ORTON'], arena=17,
                timeout=7200, shots=(600, 1800, 3600, 5400))
    s['soak'] = True
    return [s]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--suites', default='M,T,B,C,D,E,G')
    ap.add_argument('--out', default=os.path.join(g.PORT, 'runs', 'night', 'android_scenarios.jsonl'))
    a = ap.parse_args()
    gens = {'B': suite_b_phone, 'M': suite_m, 'T': g.suite_m, 'C': suite_c_sample, 'D': g.suite_d, 'E': suite_e_phone, 'G': suite_g}
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
