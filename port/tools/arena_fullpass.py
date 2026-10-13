"""Arena full pass (D10): every arena of the other games converted and played in a short match.

    py tools/arena_fullpass.py run [--games svr2007,wwe13] [--only <tag>] [--retry]
    py tools/arena_fullpass.py table            (docs/ARENA_PORT_RESULTS.md from the results)

For each arena: converts it (arena_port.py for SvR 2007-2010, svrmod wwe13 for WWE '12 / '13 / 2K14)
onto a 2011 slot, plays one muted 1-on-1 with entrances on that slot through a test game
(SVR2011_TEST_ARENA_REDIRECT, the arena test runner given by --runner), and records:
load / crash, the frame rate in the match (the log's fps lines), the converter's notes (textures
halved, entries from the host, lights moved), and a thumbnail sheet (entrance + in-ring shots).
Results: runs/fullpass/results.jsonl (one line per arena; done arenas are skipped unless --retry).
One test game at a time; waits while 6 SvR 2011 games run (the PC's limit).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
G11 = os.environ.get('SVR2011_GAME_PAC', r'D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\Game Files\pac')
ROOT = os.path.dirname(os.path.dirname(PORT))  # D:\Xbox Games Ports
EXTRACT = {'svr2007': 'SvR2007 Extract', 'svr2008': 'SvR2008 Extract', 'svr2009': 'SvR2009 Extract',
           'svr2010': 'SvR2010 Extract', 'wwe12': 'WWE12 Extract', 'wwe13': 'WWE13 Extract', 'wwe2k14': 'WWE2K14 Extract'}
NATIVE = ('wwe12', 'wwe13', 'wwe2k14')
# conversions run at idle priority (the user's CPU rule); the test game itself does not
IDLE = 0x40 if os.name == 'nt' else 0  # IDLE_PRIORITY_CLASS


def runs_dir(a):
    return a.runs


def out_dir(a):
    d = os.path.join(a.runs, 'fullpass')
    os.makedirs(d, exist_ok=True)
    return d


def arenas(game):
    src = os.path.join(ROOT, EXTRACT[game])
    if game in NATIVE:
        bg = os.path.join(src, 'pac', 'bg')
        names = sorted(n for n in os.listdir(bg) if re.fullmatch(r'bg\d\d\.pac', n.lower()))
        return [{'key': n[:-4].lower(), 'name': n[:-4].lower(), 'arena': int(n[2:4])} for n in names]
    r = subprocess.run([sys.executable, os.path.join(HERE, 'arena_port.py'), 'list', '--game', game, '--src', src],
                       capture_output=True, text=True)
    return json.loads(r.stdout)


def slot_for(game, it):
    if game in NATIVE:
        return it['arena'] if it['arena'] < 20 else 1
    return it['in_2011'] if it.get('in_2011') is not None else (it['arena'] if it['arena'] < 20 else 3)


def convert(a, game, it, slot, pac_out):
    src = os.path.join(ROOT, EXTRACT[game])
    notes = []
    if game in NATIVE:
        host = os.path.join(G11, 'bg', 'bg%02d.pac' % slot)
        r = subprocess.run([a.svrmod, 'wwe13', os.path.join(src, 'pac', 'bg', it['key'] + '.pac'), host, pac_out],
                           capture_output=True, text=True, creationflags=IDLE)
        text = (r.stdout + r.stderr).strip().splitlines()
        notes = [l.strip() for l in text if l.strip().startswith(('models', 'entry', 'texture', 'ring model', 'light sets'))
                 or 'halved' in l or l.startswith('wrote')]
        ok = r.returncode == 0 and os.path.exists(pac_out)
        if not ok:
            notes.append('convert failed: ' + (text[-1] if text else '?'))
        return ok, notes
    work = os.path.join(out_dir(a), 'conv', game + '_' + it['key'])
    r = subprocess.run([sys.executable, os.path.join(HERE, 'arena_port.py'), 'convert', '--game', game, '--src', src,
                        '--item', it['key'], '--out', work, '--base', str(slot), '--pac11', G11],
                       capture_output=True, text=True, creationflags=IDLE)
    try:
        rep = json.loads(r.stdout)
    except ValueError:
        return False, ['convert failed: ' + (r.stderr.strip().splitlines() or ['?'])[-1]]
    if not rep.get('ok'):
        return False, ['convert failed: ' + str(rep.get('error') or rep.get('notes'))]
    import shutil
    shutil.copyfile(os.path.join(rep['folder'], 'arena.pac'), pac_out)
    return True, rep.get('notes', []) + rep.get('warnings', [])


def play(a, tag, slot):
    r = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', a.runner, '-V', tag,
                        '-Slot', str(slot)], capture_output=True, text=True)
    log = os.path.join(a.runs, 's08%s.log' % tag)
    state, fps, worst = 'NO MATCH', None, None
    if os.path.exists(log):
        text = open(log, encoding='utf-8', errors='replace').read()
        if 'crash: exception' in text:
            state = 'CRASH'
            m = re.search(r'crash: guest thread \d+: lr ([0-9A-F]+)', text)
            if m:
                state += ' (lr %s)' % m.group(1)
        elif re.search(r'match: rule 0D, arena %d' % slot, text):
            state = 'OK'
        after = text.split('match: rule 0D', 1)[-1] if 'match: rule 0D' in text else ''
        vals = [float(x) for x in re.findall(r'fps: ([\d.]+) avg', after)]
        worsts = [float(x) for x in re.findall(r'worst frame ([\d.]+) ms', after)]
        if vals:
            fps = round(sum(vals) / len(vals), 1)
            worst = max(worsts) if worsts else None
    return state, fps, worst


def thumbs(a, tag):
    from PIL import Image
    base = os.path.join(a.runs, 's08' + tag)
    picks = [base + '_e%d.png' % k for k in (5, 8, 11)] + [base + '_m%d.png' % k for k in (0, 2)]
    ims = [Image.open(p).convert('RGB').resize((384, 225)) for p in picks if os.path.exists(p)]
    if not ims:
        return None
    sheet = Image.new('RGB', (384 * len(ims), 225))
    for k, im in enumerate(ims):
        sheet.paste(im, (k * 384, 0))
    path = os.path.join(out_dir(a), tag + '.jpg')
    sheet.save(path, quality=80)
    return path


def run(a):
    results = os.path.join(out_dir(a), 'results.jsonl')
    done = set()
    if os.path.exists(results) and not a.retry:
        for line in open(results, encoding='utf-8'):
            done.add(json.loads(line)['tag'])
    for game in a.games.split(','):
        for it in arenas(game):
            tag = '%s_%s' % (game, it['key'])
            if (a.only and tag != a.only) or tag in done:
                continue
            slot = slot_for(game, it)
            pac = os.path.join(a.test_dir, 's08_%s.pac' % tag)
            t0 = time.time()
            ok, notes = convert(a, game, it, slot, pac)
            rec = {'tag': tag, 'game': game, 'key': it['key'], 'name': it.get('name'), 'slot': slot, 'notes': notes}
            if not ok:
                rec['state'] = 'CONVERT FAILED'
            else:
                rec['state'], rec['fps'], rec['worst_ms'] = play(a, tag, slot)
                rec['sheet'] = thumbs(a, tag)
            rec['minutes'] = round((time.time() - t0) / 60, 1)
            with open(results, 'a', encoding='utf-8') as f:
                f.write(json.dumps(rec) + '\n')
            print('%-22s slot %2d  %-16s fps %s' % (tag, slot, rec['state'], rec.get('fps')), flush=True)


def table(a):
    results = os.path.join(out_dir(a), 'results.jsonl')
    recs = {}
    for line in open(results, encoding='utf-8'):
        r = json.loads(line)
        recs[r['tag']] = r
    lines = ['# Arenas from other games: full pass results', '',
             'Every arena converted (`tools/arena_port.py`, `svrmod wwe13`) and played in one short muted',
             '1-on-1 with entrances (`tools/arena_fullpass.py`). Look columns are filled from the shots.', '',
             '| Game | Arena | Show | Slot | Loads | fps median (lowest 5 s) | Halved | From 2011 | Notes |',
             '|---|---|---|---|---|---|---|---|---|']
    for r in sorted(recs.values(), key=lambda r: (r['game'], r['key'])):
        txt = ' '.join(r.get('notes', []))
        m = re.search(r'(\d+) textures (?:at half size|halved)', txt)
        halved = m.group(1) if m else ''
        m = re.search(r'\((\d+) from the host', txt)
        host = m.group(1) if m else ('crowd, tables, card' if 'crowd' in txt else '')
        # median of the match's 5-second fps samples (the first ones carry the load's stutters)
        fps = ''
        log = os.path.join(a.runs, 's08%s.log' % r['tag'])
        if os.path.exists(log):
            text = open(log, encoding='utf-8', errors='replace').read()
            after = text.split('match: rule 0D', 1)[-1] if 'match: rule 0D' in text else ''
            vals = sorted(float(x) for x in re.findall(r'fps: ([\d.]+) avg', after))
            if vals:
                fps = '%.1f (low %.1f)' % (vals[len(vals) // 2], vals[0])
        extra = '; '.join(n for n in r.get('notes', []) if 'lights' in n or 'failed' in n or 'no VS' in n)
        lines.append('| %s | %s | %s | %d | %s | %s | %s | %s | %s |' % (
            r['game'], r['key'], r.get('name') or '', r['slot'], r['state'], fps, halved, host, extra))
    path = os.path.join(os.path.dirname(PORT), 'docs', 'ARENA_PORT_RESULTS.md')
    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
    print(path, len(recs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['run', 'table'])
    ap.add_argument('--games', default='svr2007,svr2008,svr2009,svr2010,wwe12,wwe13,wwe2k14')
    ap.add_argument('--only')
    ap.add_argument('--retry', action='store_true')
    ap.add_argument('--runs', default=r'D:\Xbox Games Ports\SvR2011 Arenas\port\runs')
    ap.add_argument('--test-dir', default=r'D:\Xbox Games Ports\SvR2011 Arenas\port\runs\opt_wwe13b_game\Mods\ArenaTest')
    ap.add_argument('--runner', required=True, help='the arena test runner: <script>.ps1 -V <tag> -Slot <n>')
    ap.add_argument('--svrmod', default=r'D:\Xbox Games Ports\SvR2011 ArenaPort\port\out\modmaker\svrmod.exe')
    a = ap.parse_args()
    run(a) if a.cmd == 'run' else table(a)


if __name__ == '__main__':
    main()
