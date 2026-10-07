"""The OPTIONS menu's CONTROLS row and the GRAPHICS page's three tabs, with
screenshots (night worker 0, behind every window, muted):
    python gfx_menu_test.py
Shots: runs/night/shots/gfx_*.jpg; log: runs/night/logs/gfx_menu.log."""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import explore  # noqa: E402

keys = os.path.join(explore.NIGHT, 'w0', 'page_keys.txt')
open(keys, 'w').close()
os.environ['SVR2011_TEST_PAGE_KEYS'] = keys


def press(*names):
    with open(keys, 'a') as f:
        for n in names:
            f.write(n + '\n')
    time.sleep(0.4 * len(names) + 1.5)


g = explore.Game(0, 'gfx_menu')
try:
    g.send('title', 'menu MY', 'menu OPTIONS', 'wait 3000', 'where')
    ok = g.wait_for(r'script input: where:', 400)
    print('options:', ok)
    print([l for l in g.lines if 'menu group 11' in l][-1:])
    time.sleep(1)
    print(g.shot('gfx_options'))
    g.send('menu CONTROLS', 'wait 2500', 'where')
    g.wait_for(r'script input: where:', 60)
    print(g.shot('gfx_controls'))
    g.send('press B', 'wait 1500')
    time.sleep(3)
    g.send('menu GRAPHICS', 'wait 2500', 'where')
    g.wait_for(r'script input: where:', 60)
    print(g.shot('gfx_tab_display'))
    g.send('press RB', 'wait 800')
    time.sleep(2)
    print(g.shot('gfx_tab_graphics'))
    g.send('press RIGHT', 'wait 800')  # (RENDER RESOLUTION: AUTO -> 360P)
    time.sleep(2)
    print(g.shot('gfx_tab_graphics_360'))
    g.send('press LEFT', 'wait 500', 'press RB', 'wait 800')
    time.sleep(3)
    print(g.shot('gfx_tab_adv'))
    g.send('press RIGHT', 'wait 800')  # (CROWD: ON -> OFF, next start)
    time.sleep(2)
    print(g.shot('gfx_tab_adv_crowd'))
    g.send('press LEFT', 'wait 500', 'press B')
    time.sleep(2)
    g.read()
    errs, stuck = g.problems()
    print('errors:', errs[:5], 'stuck:', stuck)
finally:
    g.stop()
