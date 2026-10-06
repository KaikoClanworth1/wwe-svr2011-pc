"""Writes launcher/game_files_crc.inc: every file of the game's disc with its
size and CRC-32, for the launcher's Verify game files (launcher/verify.c).

Two files are changed by the port when the game starts (src/game_files.cpp):
  pac/menu/menu.pac   its menu table (MFLO/0000's slot, 0x25F000 + 0x6800) is
                      left out of the CRC (VERIFY_MENU_TABLE);
  pac/string.pac      the PC wording (tools/patch_strings.py): the disc's CRC
                      or the patched file's.
Reads a clean extraction of the disc ("Extract GameFiles"):
    python make_game_file_crcs.py [<disc dir>]
"""
import os
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
DISC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(TOP, "Extract GameFiles")
OUT = os.path.join(os.path.dirname(HERE), "launcher", "game_files_crc.inc")
MENU = "pac/menu/menu.pac"
MENU_TABLE = (0x25F000, 0x25F000 + 0x6800)
STRINGS = "pac/string.pac"


def crc(path, skip=None):
    """The file's CRC-32; `skip`: a byte range left out (a small file)."""
    if skip:
        d = open(path, "rb").read()
        return zlib.crc32(d[:skip[0]] + d[skip[1]:])
    c = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 22)
            if not b:
                return c
            c = zlib.crc32(b, c)


def patched_strings_crc():
    sys.path.insert(0, HERE)
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "pac"))
        import subprocess
        subprocess.run([sys.executable, os.path.join(HERE, "patch_strings.py"), DISC, tmp], check=True,
                       stdout=subprocess.DEVNULL)
        return crc(os.path.join(tmp, "pac", "string.pac"))


def main():
    rows = []
    for root, dirs, files in os.walk(DISC):
        dirs.sort()
        for n in sorted(files):
            p = os.path.join(root, n)
            rel = os.path.relpath(p, DISC).replace("\\", "/")
            size = os.path.getsize(p)
            if rel == MENU:
                rows.append((rel, size, crc(p, MENU_TABLE), 0, "VERIFY_MENU_TABLE"))
            elif rel == STRINGS:
                rows.append((rel, size, crc(p), patched_strings_crc(), "0"))
            else:
                rows.append((rel, size, crc(p), 0, "0"))
    with open(OUT, "w", newline="\n") as o:
        o.write("/* Made by tools/make_game_file_crcs.py from the game's disc: path, size, CRC-32,\n"
                " * the CRC after the port's change (0: none), flags. Don't edit. */\n")
        for rel, size, c, alt, flags in rows:
            path = rel.replace("/", "\\\\")
            o.write(f'{{L"{path}", {size}ull, 0x{c:08X}u, 0x{alt:08X}u, {flags}}},\n')
    print(f"{OUT}: {len(rows)} files")


if __name__ == "__main__":
    main()
