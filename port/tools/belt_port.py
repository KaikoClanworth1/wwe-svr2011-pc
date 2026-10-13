"""Belt reskins (D11 phase 1) as texture packs: see reskin_port.py for the commands.

    py tools/belt_port.py games | list --game svr2011 --src <game> [--pictures <dir>]
        | convert --game svr2011 --src <game> --item <key> --out <dir> --set <texture>=<picture> [...]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reskin_port  # noqa: E402

if __name__ == '__main__':
    sys.exit(reskin_port.run('belt', sys.argv[1:]))
