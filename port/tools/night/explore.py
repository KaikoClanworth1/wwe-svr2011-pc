"""Overnight menu explorer (docs/OVERNIGHT_TESTS.md, suite A): every menu,
every category, every screen behind them.

    python explore.py [--workers 4] [--monkey 150] [--max-depth 6]

1. One boot logs the game's whole menu label table ("script input: menu group
   G: a | b | ...", menu_hooks.cpp).
2. Breadth first from the main menu (group 1): for a path of labels, a game
   boots, walks it ("menu <label>" steps) and logs where it ended ("where").
   A new menu group -> its labels are queued (each group expanded once). No
   new menu -> a screen: random presses for --monkey seconds (directions, A,
   B, X, Y, shoulders, triggers, the sticks), screenshots, and the run is
   judged (crash / hang / GPU / broken picture).
Each game runs in a night.ps1 worker folder (runs\\night\\w<N>, made by
night.ps1 -Setup), muted, on screen 2 behind every window (never activated).
Results -> runs\\night\\results.jsonl (suite "A"), tree -> runs\\night\\menu_tree.json.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import queue
import random
import re
import subprocess
import threading
import time

PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
NIGHT = os.path.join(PORT, 'runs', 'night')
RESULTS = os.path.join(NIGHT, 'results.jsonl')
LOGS = os.path.join(NIGHT, 'logs')
SHOTS = os.path.join(NIGHT, 'shots')
ERR = re.compile(r"SvR 2011 crash|\[FATAL\]|Unhandled guest access|device lost|DEVICE_REMOVED|can't draw the game|not in its list")
lock = threading.Lock()

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
user32.SetProcessDPIAware()


# ---- windows ----------------------------------------------------------------
def find_window(pid):
    found = []
    cb_t = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(h, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(h):
            buf = ctypes.create_unicode_buffer(128)
            user32.GetWindowTextW(h, buf, 128)
            if 'SmackDown' in buf.value:
                found.append(h)
                return False
        return True
    user32.EnumWindows(cb_t(cb), 0)
    return found[0] if found else None


def shot(hwnd, path):
    """PrintWindow -> JPEG (half size); returns (mean, spread) of the brightness."""
    from PIL import Image
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return None
    hdc_w = user32.GetWindowDC(hwnd)
    hdc = gdi32.CreateCompatibleDC(hdc_w)
    bmp = gdi32.CreateCompatibleBitmap(hdc_w, w, h)
    gdi32.SelectObject(hdc, bmp)
    user32.PrintWindow(hwnd, hdc, 2)

    class BMI(ctypes.Structure):
        _fields_ = [('biSize', wt.DWORD), ('biWidth', wt.LONG), ('biHeight', wt.LONG), ('biPlanes', wt.WORD),
                    ('biBitCount', wt.WORD), ('biCompression', wt.DWORD), ('biSizeImage', wt.DWORD),
                    ('biXPelsPerMeter', wt.LONG), ('biYPelsPerMeter', wt.LONG), ('biClrUsed', wt.DWORD),
                    ('biClrImportant', wt.DWORD)]
    bi = BMI(ctypes.sizeof(BMI), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(hdc)
    user32.ReleaseDC(hwnd, hdc_w)
    img = Image.frombuffer('RGB', (w, h), buf, 'raw', 'BGRX', 0, 1)
    small = img.resize((w // 2, h // 2))
    small.save(path, quality=70)
    g = img.convert('L').resize((32, 18))
    px = list(g.getdata())
    mean = sum(px) / len(px)
    sd = (sum((p - mean) ** 2 for p in px) / len(px)) ** 0.5
    return round(mean, 1), round(sd, 1)


# ---- one game ---------------------------------------------------------------
class Game:
    def __init__(self, worker, name):
        self.worker = worker
        self.dir = os.path.join(NIGHT, 'w%d' % worker)
        self.log = os.path.join(LOGS, name + '.log')
        self.input = os.path.join(self.dir, 'input.txt')
        self.pos = 0
        self.lines = []
        os.makedirs(LOGS, exist_ok=True)
        if os.path.exists(self.log):
            os.remove(self.log)
        open(self.input, 'w').close()
        env = dict(os.environ)
        for k in list(env):
            if k.startswith('SVR2011_TEST') or k in ('SVR2011_ROUTE',):
                del env[k]
        env.update({'SVR2011_INPUT_FILE': self.input, 'SVR2011_CONFIG': os.path.join(self.dir, 'test_config.toml'),
                    'SVR2011_USER_DATA': os.path.join(self.dir, 'UserData'), 'SVR2011_WINDOW_BEHIND': '1',
                    'SDL_WINDOW_ACTIVATE_WHEN_SHOWN': '0', 'SDL_WINDOW_ACTIVATE_WHEN_RAISED': '0'})
        args = [os.path.join(self.dir, 'svr2011.exe'), '--log_file=' + self.log, '--log_level=info',
                '--audio_mute=true', '--fullscreen=false', '--monitor=2', '--show_fps=false']
        si = subprocess.STARTUPINFO()
        si.dwFlags = subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 4  # SW_SHOWNOACTIVATE
        self.p = subprocess.Popen(args, cwd=self.dir, env=env, startupinfo=si,
                                  creationflags=subprocess.CREATE_NO_WINDOW)
        self.start = time.time()
        self.hwnd = None

    def send(self, *steps):
        with open(self.input, 'a', encoding='utf-8') as f:
            for s in steps:
                f.write(s + '\n')

    def read(self):
        if not os.path.exists(self.log):
            return []
        try:
            with open(self.log, 'rb') as f:
                f.seek(self.pos)
                data = f.read()
                self.pos += len(data)
        except OSError:
            return []
        new = data.decode('utf-8', 'replace').splitlines()
        self.lines += new
        return new

    def wait_for(self, pattern, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end and self.p.poll() is None:
            for line in self.read():
                if rx.search(line):
                    return line
            time.sleep(0.5)
        return None

    def park(self):
        if not self.hwnd:
            self.hwnd = find_window(self.p.pid)
            if self.hwnd:  # screen 2, behind every window, never activated
                user32.SetWindowPos(self.hwnd, 1, 1920 + 40 * self.worker, 40 * self.worker, 0, 0, 0x1 | 0x10)

    def shot(self, name):
        self.park()
        if not self.hwnd:
            return None
        os.makedirs(SHOTS, exist_ok=True)
        path = os.path.join(SHOTS, name + '.jpg')
        try:
            r = shot(self.hwnd, path)
        except Exception:
            return None
        return None if not r else {'file': os.path.basename(path), 'mean': r[0], 'sd': r[1]}

    def stop(self):
        if self.p.poll() is None:
            self.p.kill()
            try:
                self.p.wait(15)
            except Exception:
                pass

    def problems(self):
        errs = [l[:240] for l in self.lines if ERR.search(l)][:12]
        stuck = max([int(m.group(1)) for l in self.lines for m in [re.search(r'world update stuck (\d+) s', l)] if m] or [0])
        return errs, stuck

    def fps(self):
        v = [float(m.group(1)) for l in self.lines for m in [re.search(r'fps: ([\d.]+) avg', l)] if m]
        return v


# ---- the explorer -----------------------------------------------------------
BUTTONS = (['UP', 'DOWN', 'LEFT', 'RIGHT'] * 10 + ['A'] * 8 + ['B'] * 3 + ['X', 'Y'] * 2 +
           ['LB', 'RB', 'LT', 'RT', 'START', 'BACK'])


def monkey(g, seconds, tag):
    shots = []
    end = time.time() + seconds
    rng = random.Random(hash(tag) & 0xFFFF)
    last_shot = 0
    n = 0
    while time.time() < end and g.p.poll() is None:
        b = rng.choice(BUTTONS)
        if b in ('LT', 'RT'):
            g.send('trigger %s 255 200' % b[0])
        elif rng.random() < 0.05:
            g.send('stick L %d %d 600' % (rng.choice([-32000, 0, 32000]), rng.choice([-32000, 0, 32000])))
        else:
            g.send('press %s 120' % b)
        n += 1
        time.sleep(rng.uniform(0.5, 1.1))
        g.read()
        if time.time() - last_shot > seconds / 3:
            s = g.shot('%s_m%d' % (tag, len(shots)))
            if s:
                shots.append(s)
            last_shot = time.time()
    return shots, n


def record(row):
    with lock:
        with open(RESULTS, 'a', encoding='utf-8') as f:
            f.write(json.dumps(row) + '\n')


def judge(g, passed_alive, shots):
    errs, stuck = g.problems()
    alive = g.p.poll() is None
    if any('crash' in e or 'FATAL' in e for e in errs) or (not alive and passed_alive):
        return 'crash', errs, stuck
    if any('device lost' in e or 'DEVICE_REMOVED' in e or "can't draw" in e for e in errs):
        return 'gpu', errs, stuck
    if stuck >= 30:
        return 'hang', errs, stuck
    if len([s for s in shots if s['sd'] < 3 and s['mean'] < 10]) >= 2:
        return 'black', errs, stuck
    if errs:
        return 'warn', errs, stuck
    return 'pass', errs, stuck


def discover(worker):
    """Boot once: the label table and the main menu group."""
    g = Game(worker, 'A_discover')
    try:
        g.send('title', 'where')
        g.wait_for(r'script input: where:', 300)
        time.sleep(2)
        g.read()
    finally:
        g.stop()
    groups = {}
    for l in g.lines:
        m = re.search(r'script input: menu group ([0-9A-F]+): (.*)$', l)
        if m:
            groups[int(m.group(1), 16)] = [t.strip() for t in m.group(2).split(' | ')]
    return groups


def visit(worker, path, groups, args, expanded, q, tree):
    tag = 'A_' + re.sub(r'[^A-Za-z0-9]+', '_', '>'.join(path))[:80]
    g = Game(worker, tag)
    t0 = time.time()
    row = {'id': tag, 'suite': 'A', 'name': ' > '.join(path), 'worker': worker}
    try:
        g.send('title', 'where')
        if not g.wait_for(r'script input: where:', 300):
            raise RuntimeError('no main menu')
        steps = ['menu ' + p for p in path] + ['wait 4000', 'where']
        g.send(*steps)
        where = g.wait_for(r'script input: where:', 60 + 40 * len(path))
        group = None
        m = re.search(r'where: group ([0-9A-F]+)', where or '')
        if m:
            group = int(m.group(1), 16)
        took = [l for l in g.lines if 'the game took' in l or 'not in the open menu' in l or 'never got' in l]
        last_group = None  # the group the last label was in
        for gid, labels in groups.items():
            if path[-1] in labels:
                last_group = gid
        is_menu = group is not None and group != last_group and group in groups and not took
        row['navigation'] = 'ok' if not took else 'off: ' + took[0][-160:]
        if is_menu and len(path) < args.max_depth:
            with lock:
                tree[' > '.join(path)] = '%X' % group
                fresh = group not in expanded
                expanded.add(group)
            row['kind'] = 'menu %X' % group
            if fresh:
                for lab in groups[group]:
                    if lab:
                        q.put(path + [lab])
            shots = [s for s in [g.shot(tag + '_0')] if s]
            time.sleep(2)
        else:
            row['kind'] = 'screen'
            s0 = g.shot(tag + '_0')
            shots, presses = monkey(g, args.monkey, tag)
            if s0:
                shots.insert(0, s0)
            row['presses'] = presses
        time.sleep(1)
        g.read()
        result, errs, stuck = judge(g, True, shots)
        if took and result == 'pass':
            result = 'nav'
        fps = g.fps()
        row.update({'result': result, 'errors': errs, 'stuckMax': stuck, 'shots': shots,
                    'fpsMinAvg': min(fps[3:]) if len(fps) > 3 else None,
                    'fpsMeanAvg': round(sum(fps[3:]) / len(fps[3:]), 1) if len(fps) > 3 else None})
    except Exception as e:
        g.read()
        errs, stuck = g.problems()
        row.update({'result': 'crash' if g.p.poll() is not None else 'error', 'errors': errs + [str(e)],
                    'stuckMax': stuck, 'shots': []})
    finally:
        g.stop()
    row['seconds'] = int(time.time() - t0)
    row['log'] = os.path.basename(g.log)
    row['time'] = time.strftime('%Y-%m-%dT%H:%M:%S')
    record(row)
    print('%s A %-60s %-6s %ss' % (time.strftime('%H:%M:%S'), row['name'][:60], row['result'], row['seconds']),
          flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--workers', type=int, default=4)
    ap.add_argument('--monkey', type=int, default=150)
    ap.add_argument('--max-depth', type=int, default=6)
    args = ap.parse_args()
    groups = discover(0)
    print(len(groups), 'menu groups;', 'main menu:', groups.get(1), flush=True)
    with open(os.path.join(NIGHT, 'menu_groups.json'), 'w', encoding='utf-8') as f:
        json.dump({'%X' % k: v for k, v in groups.items()}, f, indent=1)
    q = queue.Queue()
    expanded = {1}
    tree = {}
    for lab in groups.get(1, []):
        if lab:
            q.put([lab])
    busy = [0]

    def worker_loop(w):
        while True:
            try:
                path = q.get(timeout=30)
            except queue.Empty:
                with lock:
                    if busy[0] == 0:
                        return
                continue
            with lock:
                busy[0] += 1
            try:
                visit(w, path, groups, args, expanded, q, tree)
            finally:
                with lock:
                    busy[0] -= 1
                q.task_done()

    threads = [threading.Thread(target=worker_loop, args=(w,), daemon=True) for w in range(args.workers)]
    for i, t in enumerate(threads):
        t.start()
        time.sleep(5 * (i > 0))
    for t in threads:
        t.join()
    with open(os.path.join(NIGHT, 'menu_tree.json'), 'w', encoding='utf-8') as f:
        json.dump(tree, f, indent=1)
    print('explorer done', flush=True)


if __name__ == '__main__':
    main()
