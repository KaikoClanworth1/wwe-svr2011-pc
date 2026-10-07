"""The overnight report: runs/night/results.jsonl (+ android_results.jsonl,
mali_results.jsonl) and runs/night/bugs.md (the night's bug log: what was
found, who fixed it, the commits) -> runs/night/REPORT.md.

    python report.py
"""
import collections
import json
import os

PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
NIGHT = os.path.join(PORT, 'runs', 'night')
SUITES = {'A': 'Menus and screens (explorer)', 'B': 'Match types (CPU vs CPU to the finish)',
          'C': 'Superstars', 'D': 'Arenas', 'E': 'Settings variants', 'F': 'Weak hardware', 'G': 'Soak'}
BAD = ('crash', 'hang', 'gpu', 'black', 'timeout', 'error')


def load(name, platform):
    path = os.path.join(NIGHT, name)
    rows = {}
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            try:
                r = json.loads(line)
            except ValueError:
                continue
            r.setdefault('platform', platform)
            # A game the random presses closed through the main menu's EXIT isn't a crash.
            if r.get('result') == 'crash' and not r.get('errors') and r.get('log'):
                lp = os.path.join(NIGHT, 'logs', r['log'])
                if os.path.exists(lp):
                    tail = open(lp, encoding='utf-8', errors='replace').read()[-4000:]
                    if 'EXIT (group 1' in tail and 'Window closing' in tail and 'SvR 2011 crash' not in tail:
                        r['result'] = 'exited'
            rows[(platform, r.get('id'))] = r  # (a rerun replaces the earlier result)
    return list(rows.values())


def main():
    rows = load('results.jsonl', 'PC') + load('android_results.jsonl', 'Android (Fold)') + \
        load('mali_results.jsonl', 'Android (Mali tablet)')
    out = ['# Overnight test report', '']
    bugs = os.path.join(NIGHT, 'bugs.md')
    if os.path.exists(bugs):
        out += [open(bugs, encoding='utf-8').read().strip(), '']
    out += ['## Totals', '', '| Platform | Suite | Runs | Pass | Problems |', '| --- | --- | --- | --- | --- |']
    by = collections.defaultdict(list)
    for r in rows:
        by[(r['platform'], r.get('suite', '?'))].append(r)
    for (plat, suite), rs in sorted(by.items()):
        c = collections.Counter(r.get('result') for r in rs)
        probs = ', '.join('%s %d' % (k, v) for k, v in c.items() if k != 'pass')
        out.append('| %s | %s | %d | %d | %s |' % (plat, SUITES.get(suite, suite), len(rs), c.get('pass', 0),
                                                 probs or '-'))
    out += ['', '## Problems', '']
    for r in sorted((r for r in rows if r.get('result') in BAD + ('warn', 'nav', 'invalid')),
                    key=lambda r: (BAD.index(r['result']) if r['result'] in BAD else 9, r['platform'], r['id'])):
        out.append('### %s - %s - %s (%s)' % (r['result'].upper(), r['platform'], r.get('name', r['id']), r['id']))
        if r.get('errors'):
            out += ['```'] + [e.strip() for e in r['errors'][:6]] + ['```']
        if r.get('stuckMax'):
            out.append('- stuck up to %s s' % r['stuckMax'])
        if r.get('navigation') and r['navigation'] != 'ok':
            out.append('- navigation: %s' % r['navigation'])
        shots = [(s.get('file') or s.get('path') or s.get('name') or str(s)) if isinstance(s, dict) else str(s)
                 for s in (r.get('shots') or [])]
        shots = [os.path.basename(s) for s in shots]
        if shots:
            out.append('- shots: ' + ', '.join(shots[:4]))
        out.append('- log: %s' % r.get('log', '?'))
        out.append('')
    slow = sorted((r for r in rows if r.get('fpsMinAvg') is not None), key=lambda r: r['fpsMinAvg'])[:25]
    out += ['## Slowest scenes (lowest 5-second average fps)', '', '| Platform | Scenario | Min avg fps | Mean | Worst frame ms |',
            '| --- | --- | --- | --- | --- |']
    for r in slow:
        out.append('| %s | %s | %s | %s | %s |' % (r['platform'], r.get('name', r['id']), r['fpsMinAvg'],
                                                   r.get('fpsMeanAvg'), r.get('worstFrameMs')))
    path = os.path.join(NIGHT, 'REPORT.md')
    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
    print(len(rows), 'results ->', path)


if __name__ == '__main__':
    main()
