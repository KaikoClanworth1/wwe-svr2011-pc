"""A problem report's main save into a private test user data folder (for svrtest --userdata).

    py port/tools/report_save.py <report.zip | report folder> <private userdata> [--dlc <userdata with a DLC copy>]
                                 [--keep-caw-records]

The report (the launcher's Report a problem zip, or a folder it was unpacked to) holds the player's main save under
save/: SaveData.dat, .info/SaveData.dat.header, .universe/... - not the Created Superstars (.cas), Paint Tool, logos
or highlight reels. It goes to <userdata>\\Saves\\ (made fresh: an existing Saves folder there is moved aside to
Saves.before-<time>).

SaveData.dat alone stops the game at "The Created Superstar save data is either damaged or missing ... overwriting
... YES / NO" before the main menu: its 50 Created Superstar records name .cas files the report doesn't carry. So
the records are emptied (0x6BC bytes at 0x13FB8 + slot * 0x6BC, all zero = a free slot - launcher/caw_import.c) and
the save's checksum made again (+0x18, big-endian: the byte sum of 0x1C..0x812DC) - the same the game's own YES
does. --keep-caw-records leaves them (to see the prompt).

--dlc: copies <that userdata>\\0000000000000000 (the DLC) in - real files, never links (the game writes into its
installed DLC). Never Game Files' UserData: the target must not be inside Game Files.
Exit 0 ok, 2 bad input. Prints what it did.
"""
import io
import os
import shutil
import sys
import time
import zipfile

REC_BASE, REC_SIZE, REC_COUNT = 0x13FB8, 0x6BC, 50
SUM_FROM, SUM_TO, SUM_AT = 0x1C, 0x812DC, 0x18


class Bad(Exception):
    pass


def read_save(src):
    """-> {relative path under save/: bytes}"""
    files = {}
    if os.path.isfile(src) and zipfile.is_zipfile(src):
        with zipfile.ZipFile(src) as z:
            for n in z.namelist():
                if n.startswith('save/') and not n.endswith('/'):
                    files[n[5:]] = z.read(n)
    elif os.path.isdir(src):
        root = os.path.join(src, 'save')
        for d, _, fs in os.walk(root):
            for f in fs:
                full = os.path.join(d, f)
                files[os.path.relpath(full, root).replace(os.sep, '/')] = open(full, 'rb').read()
    else:
        raise Bad('%s is neither a report zip nor a folder' % src)
    if 'SaveData.dat' not in files:
        raise Bad('the report has no save/SaveData.dat (sent without "Include my main save"?)')
    for name in files:   # (a name, not a path out of the folder)
        if name.startswith('/') or '..' in name.split('/') or ':' in name:
            raise Bad('a bad name in the report: %s' % name)
    return files


def empty_caw_records(sd):
    sd = bytearray(sd)
    if len(sd) < SUM_TO:
        raise Bad('SaveData.dat is too short (%d bytes)' % len(sd))
    used = [k for k in range(REC_COUNT) if any(sd[REC_BASE + k * REC_SIZE:REC_BASE + (k + 1) * REC_SIZE])]
    for k in used:
        sd[REC_BASE + k * REC_SIZE:REC_BASE + (k + 1) * REC_SIZE] = bytes(REC_SIZE)
    total = sum(sd[SUM_FROM:SUM_TO]) & 0xFFFFFFFF
    sd[SUM_AT:SUM_AT + 4] = total.to_bytes(4, 'big')
    return bytes(sd), used


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    keep = '--keep-caw-records' in argv
    dlc = argv[argv.index('--dlc') + 1] if '--dlc' in argv else None
    if dlc in args:
        args.remove(dlc)
    try:
        if len(args) != 2:
            raise Bad('usage: report_save.py <report.zip|folder> <private userdata> [--dlc <userdata>] [--keep-caw-records]')
        src, ud = args
        if 'game files' in os.path.abspath(ud).lower():
            raise Bad('the target is inside Game Files - use a private test user data folder')
        files = read_save(src)
        saves = os.path.join(ud, 'Saves')
        if os.path.exists(saves):
            aside = saves + '.before-' + time.strftime('%Y%m%d-%H%M%S')
            os.rename(saves, aside)
            print('the folder\'s old Saves moved to', aside)
        if not keep:
            files['SaveData.dat'], used = empty_caw_records(files['SaveData.dat'])
            print('Created Superstar records emptied: %d (slots %s); checksum made again' % (len(used), used))
        for name, data in files.items():
            out = os.path.join(saves, *name.split('/'))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, 'wb') as f:
                f.write(data)
            print('save/%s -> %s (%d bytes)' % (name, out, len(data)))
        if dlc:
            s, d = os.path.join(dlc, '0000000000000000'), os.path.join(ud, '0000000000000000')
            if not os.path.isdir(s):
                raise Bad('--dlc %s has no 0000000000000000 folder' % dlc)
            if not os.path.exists(d):
                shutil.copytree(s, d, symlinks=False)
                print('DLC copied from', s)
        print('ok: svrtest ... --userdata "%s"' % ud)
        return 0
    except Bad as e:
        print('error:', e)
        return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
