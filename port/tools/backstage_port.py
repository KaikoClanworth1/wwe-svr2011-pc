"""Backstage areas from other games -> SvR 2011 backstage mods (Mods/Backstage/<name>).

    py tools/backstage_port.py games
    py tools/backstage_port.py list --game <id> --src <ISO or extract folder>
    py tools/backstage_port.py convert --game <id> --src <...> --item <key> --out <mod folder>
                                       --pac11 <2011 game folder> [--name <row name>] [--id <manifest id>]
    (list also takes --pac11 and ignores it; convert's id defaults to the --out folder's name, letters,
    digits and _ - the launcher installs a mod under Mods/<type>/<id>)

stdout is JSON only; logs and `progress <0-100> <text>` lines go to stderr.
Exit codes: 0 ok, 2 bad input, 3 conversion failed.

- hctp (WWE SmackDown! Here Comes the Pain, PS2): its seven backstage rooms (BG.PAC STG 0030-0036) with
  their props and interactions - the room code is in the game (src/hctp_room.cpp, manifest hctp=<room>);
  this builds the room into a copy of 2011's bg78 (parking slot), its gimmick package (props, climb spots,
  converted motions) and hctp.txt (tools/hctp/build_room.py; docs/HCTP_BACKSTAGE.md). Each item is its own
  row in the BACKSTAGE list. Needs the player's 2011 pac/bg/bg78.pac and pac/gm.pac (--pac11).
"""
import argparse
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'hctp'))
import discfs  # noqa: E402

GAMES = [
    {'id': 'hctp', 'name': "WWE SmackDown! Here Comes the Pain", 'platform': 'ps2',
     'disc_hint': 'SLES_520.36', 'disc_hints': ['SLES_520.36', 'SLUS_207.87'], 'status': 'ready'},
]
# HCTP room -> what it is (re_gimmicks.md / re_rooms31_36.md; 36 is 35 without the flood water)
HCTP_ROOMS = {
    30: ('Back Entrance', 'the mini cage: walls to tear down, a roof to fall through, a cart, valves, a crate'),
    31: ('Parking Lot', 'cars to wreck, a forklift to drive, a generator, oil drums, the trailer'),
    32: ('Training Room', 'lockers, showers, a toilet stall, shelves, a TV, a wall to break, a sandbag, treadmills'),
    33: ('Times Square', 'traffic, a helicopter, lamp-post poles, shop windows, the upper levels'),
    34: ('Outside the Arena', 'the street outside (the fire extinguisher is 2011\'s own)'),
    35: ('Fountain Square', 'lion statues to climb, fountains to throw people into'),
    36: ('Fountain Square (dry)', 'room 35 as HCTP\'s second copy'),
}


class BadInput(Exception):
    pass


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def progress(p, text):
    print('progress %d %s' % (p, text), file=sys.stderr, flush=True)


def hctp_check(disc):
    for p in ('PAC/BG.PAC', 'PAC/GM.PAC', 'PAC/M.PAC'):
        if not disc.exists(p):
            raise BadInput('not WWE SmackDown! Here Comes the Pain: no %s' % p)


def hctp_list(disc):
    import hctp_unpack
    hctp_check(disc)
    stg = {name for (g, name) in hctp_unpack.dpac(disc.read('PAC/BG.PAC', 0, 0x4000)) if g == 'STG'}
    return [{'key': 'hctp%d' % r, 'name': n, 'room': r, 'about': about, 'status': 'ready' if '%04d' % r in stg else 'missing'}
            for r, (n, about) in sorted(HCTP_ROOMS.items())]


def pac_folder(pac11):
    """--pac11: the 2011 game folder (or its pac folder) -> the pac folder"""
    for p in (os.path.join(pac11, 'pac'), pac11):
        if os.path.isfile(os.path.join(p, 'bg', 'bg78.pac')) and os.path.isfile(os.path.join(p, 'gm.pac')):
            return p
    raise BadInput('--pac11 %s: no pac/bg/bg78.pac and pac/gm.pac (the 2011 game folder)' % pac11)


def hctp_convert(disc, item, out, pac11, name, mod_id=None):
    import hctp_unpack
    import build_room
    hctp_check(disc)
    try:
        room = int(item.lower().replace('hctp', ''))
    except ValueError:
        room = -1
    if room not in HCTP_ROOMS:
        raise BadInput('unknown item %r (hctp30 .. hctp36)' % item)
    pac = pac_folder(pac11)
    work = tempfile.mkdtemp(prefix='backstage_port_')
    try:
        progress(2, 'reading the disc')
        unpacked = os.path.join(work, 'unpacked')
        hctp_unpack.unpack(disc, unpacked, [room])
        os.makedirs(out, exist_ok=True)
        rep = build_room.build(room, unpacked, pac, work, out, name or '%s (HCTP)' % HCTP_ROOMS[room][0],
                               progress=lambda p, t: progress(5 + p * 0.95, t), mod_id=mod_id)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return {'ok': True, 'out': out, 'opens': 'backstage', 'name': rep['name'], 'room': room, 'id': rep['id'],
            'files': rep['files'], 'warnings': [], 'dropped': [],
            'notes': ['installs as Mods/Backstage/%s; plays from its own row in BACKSTAGE' % os.path.basename(out.rstrip('/\\')),
                      HCTP_ROOMS[room][1]]}


def main(argv):
    ap = argparse.ArgumentParser(prog='backstage_port.py')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('games')
    p = sub.add_parser('list')
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--pac11')  # (accepted, not needed to list)
    p = sub.add_parser('convert')
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--item', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--pac11', required=True)
    p.add_argument('--name')
    p.add_argument('--id')  # (the manifest id; default: the --out folder's name)
    try:
        a = ap.parse_args(argv)
    except SystemExit:
        print(json.dumps({'ok': False, 'error': 'bad arguments'}))
        return 2
    if a.cmd == 'games':
        print(json.dumps(GAMES))
        return 0
    try:
        if not any(g['id'] == a.game for g in GAMES):
            raise BadInput('unknown game %r' % a.game)
        if not os.path.exists(a.src):
            raise BadInput('--src %s: not found' % a.src)
        with discfs.open(a.src) as disc:
            if a.cmd == 'list':
                print(json.dumps(hctp_list(disc)))
                return 0
            rep = hctp_convert(disc, a.item, a.out, a.pac11, a.name, a.id)
            with open(os.path.join(a.out, 'report.json'), 'w') as f:
                json.dump(rep, f, indent=1)
            print(json.dumps(rep))
            return 0
    except BadInput as e:
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 2
    except Exception as e:  # noqa: BLE001 - reported to the Mod Maker
        log('failed: %r' % e)
        print(json.dumps({'ok': False, 'error': str(e), 'out': None, 'opens': 'backstage', 'warnings': [],
                          'dropped': [], 'notes': []}))
        return 3


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
