"""Rewrites the game's text in pac/string.pac for the PC port: "Xbox LIVE"
becomes "Online", the Xbox 360 console / storage device / Dashboard become
the PC, the save folder and the Music folder.

The strings are null-terminated and the patched ones only get shorter, so each
string is rewritten in place and padded with nulls (offsets stay valid).
The same table is applied to the DLC's own string.pac at startup
(src/dlc.cpp, kStringPatches) - keep the two in sync.
Reads the original from "Extract GameFiles" and writes "Game Files" (idempotent):
    python patch_strings.py [<disc dir> <game dir>]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
DISC = sys.argv[1] if len(sys.argv) > 2 else os.path.join(TOP, "Extract GameFiles")
GAME = sys.argv[2] if len(sys.argv) > 2 else os.path.join(TOP, "Game Files")
FILE = os.path.join("pac", "string.pac")

# Whole strings (menu labels are upper case).
LABELS = {b"Xbox LIVE": b"ONLINE"}
# Inside strings, in this order (longest first where they overlap).
PHRASES = [
    # English
    (b"Please don't turn off \nyour Xbox 360 console.", b"Please don't close \nthe game."),
    (b"Please don't turn off your Xbox 360 console\n", b"Please don't close the game\n"),
    (b"Please don't turn off your Xbox 360 console.", b"Please don't close the game."),
    (b"your Xbox 360 console.", b"the game."),
    (b"If you rip a music CD to the console, you can set your favorite music as background music.",
     b"Put .mp3 files in the game's Music folder to use your own music as background music."),
    (b"create a Playlist containing\nonly that song through the Xbox Dashboard.",
     b"put only that song in its own\nfolder in the game's Music folder."),
    (b"storage device", b"save folder"),
    # CAW logos: the port allows 10 High Resolution logos (src/caw_logos.cpp)
    (b"A maximum of 2 different High Resolution logos can be",
     b"Up to 10 different High Resolution logos can be"),
    (b"play on Xbox LIVE", b"play online"),
    (b"on Xbox LIVE", b"online"),
    (b"Xbox LIVE", b"Online"),
    # German
    (b"deine\nXbox 360 Konsole", b"deinen\nPC"),
    (b"deine Xbox 360 Konsole", b"deinen PC"),
    (b"Xbox 360 Konsole", b"PC"),
    # Spanish
    (b"tu Consola Xbox 360", b"tu PC"),
    (b"la Consola Xbox 360", b"el PC"),
    (b"Consola Xbox 360", b"PC"),
    # Italian
    (b"la console Xbox 360", b"il PC"),
    (b"console Xbox 360", b"PC"),
    # French
    (b"votre Console Xbox 360", b"votre PC"),
    (b"Console Xbox 360", b"PC"),
    # Japanese ("Xbox 360 hon-tai": the console unit)
    ("Xbox 360 本体".encode("utf-8"), "PC".encode("utf-8")),
    # anything left
    (b"Xbox 360", b"PC"),
]
TRIGGERS = [b"Xbox LIVE", b"Xbox 360", b"storage device", b"music CD", b"Xbox Dashboard",
            b"A maximum of 2 different High Resolution"]


def patch_string(s: bytes) -> bytes:
    if s in LABELS:
        return LABELS[s]
    for old, new in PHRASES:
        s = s.replace(old, new)
    return s


def patch(data: bytearray) -> int:
    count = 0
    done = set()
    for trigger in TRIGGERS:
        at = data.find(trigger)
        while at >= 0:
            start = data.rfind(b"\x00", 0, at) + 1
            end = data.find(b"\x00", at)
            if end < 0:
                break
            if start not in done:
                done.add(start)
                old = bytes(data[start:end])
                new = patch_string(old)
                if new != old and len(new) <= len(old):
                    data[start:end] = new + b"\x00" * (len(old) - len(new))
                    count += 1
            at = data.find(trigger, end)
    return count


def main() -> int:
    src = os.path.join(DISC, FILE)
    dst = os.path.join(GAME, FILE)
    data = bytearray(open(src, "rb").read())
    count = patch(data)
    if os.path.exists(dst) and open(dst, "rb").read() == data:
        print(f"{FILE}: already patched ({count} strings)")
        return 0
    tmp = dst + ".tmp"
    open(tmp, "wb").write(data)
    os.replace(tmp, dst)
    print(f"{FILE}: {count} strings rewritten for the PC")
    return 0


if __name__ == "__main__":
    sys.exit(main())
