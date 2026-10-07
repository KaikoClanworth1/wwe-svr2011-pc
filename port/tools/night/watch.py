"""Prints one line per NEW real problem in the night's results (PC + Android),
skipping games the random presses closed through the main menu's EXIT.
For a Monitor: python watch.py  (runs until killed; polls every 20 s)."""
import json
import os
import time

NIGHT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'runs', 'night'))
FILES = {'results.jsonl': ('PC', 'logs'), 'android_results.jsonl': ('Fold', 'android_logs'),
         'mali_results.jsonl': ('Mali', 'android_logs')}
BAD = {'crash', 'hang', 'gpu', 'black', 'timeout', 'error', 'mode-missing'}
seen = {}


def exited(row, logdir):
    lp = os.path.join(NIGHT, logdir, row.get('log', '') or '')
    if row.get('errors') or not os.path.exists(lp):
        return False
    tail = open(lp, encoding='utf-8', errors='replace').read()[-6000:]
    return 'EXIT (group 1' in tail and 'Window closing' in tail and 'SvR 2011 crash' not in tail


first = True
while True:
    for name, (plat, logdir) in FILES.items():
        p = os.path.join(NIGHT, name)
        if not os.path.exists(p):
            continue
        lines = open(p, encoding='utf-8', errors='replace').read().splitlines()
        start = seen.get(name, 0)
        for line in lines[start:]:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if first:
                continue
            # (a timeout with no errors and no stuck update = a long match still being played)
            quiet_timeout = r.get('result') == 'timeout' and not r.get('errors') and not r.get('stuckMax')
            if r.get('result') in BAD and not quiet_timeout and not (r.get('result') == 'crash' and exited(r, logdir)):
                err = (r.get('errors') or [''])[0][-150:]
                print('%s %s %s - %s (%s) %s' % (plat, r.get('result'), r.get('id'), r.get('name'), r.get('log'), err),
                      flush=True)
        seen[name] = len(lines)
    if first:
        print('watching from: ' + ', '.join('%s=%d' % kv for kv in seen.items()), flush=True)
    first = False
    time.sleep(20)
