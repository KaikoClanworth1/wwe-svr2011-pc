"""Makes assets/svr2011.ico, the program icon, from your own game files.

The icon is the game's Xbox 360 title image (the 64 x 64 dashboard tile). It
is kept out of the repository because it is game art; the disc's `nxeart`
package carries it in its header (STFS title thumbnail at 0x571A), so it can
be rebuilt from an install:

    python tools/make_icon.py "<game folder or extracted disc>"

Needs Pillow (pip install pillow). The build embeds the icon when the file
exists and builds without one otherwise.
"""
import os
import struct
import sys
from io import BytesIO

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "assets", "svr2011.ico")
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = os.path.join(sys.argv[1], "nxeart")
    data = open(path, "rb").read()
    if data[:4] not in (b"PIRS", b"LIVE", b"CON "):
        print(f"{path}: not an STFS package")
        return 1
    size = struct.unpack_from(">I", data, 0x1716)[0]
    png = data[0x571A:0x571A + size]
    if not size or png[:8] != b"\x89PNG\r\n\x1a\n":
        print(f"{path}: no title image")
        return 1
    src = Image.open(BytesIO(png)).convert("RGBA")
    big = src.resize((256, 256), Image.LANCZOS)
    imgs = [src if s == src.width else src.resize((s, s), Image.LANCZOS) for s in SIZES]
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    big.save(OUT, sizes=[(s, s) for s in SIZES], append_images=imgs)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
