"""Adds menu entries (pac/menu/menu.pac): GRAPHICS to MY WWE -> OPTIONS and
EXIT to the main menu.

The main menus are a table of 0x74-byte records in menu.pac entry MFLO/0000
(big-endian; parsed by sub_82BAA6E8):
  +0x00 label string id   +0x18 group   +0x1C node id (ascending)
  +0x38 parent node       +0x3C flags (1 submenu, 2 last of group, 4 screen)
  +0x54 screen id
A group's records are consecutive, in display order; the "last" flag ends it.
OPTIONS (node 0x1C) is group 0x11: MATCH CREATOR, GAMEPLAY OPTIONS, SAVE DATA
MANAGER, CREDITS, CHEAT CODES. GRAPHICS is inserted after CHEAT CODES as a copy
of it (screen 0x524, so without the port's hooks it would open Cheat Codes)
with its own label id; the port supplies the label and opens its own page
(src/menu_hooks.cpp).

Reads the original from "Extract GameFiles" and writes "Game Files" (idempotent):
    python patch_menu.py [<disc dir> <game dir>]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(os.path.dirname(HERE))
DISC = sys.argv[1] if len(sys.argv) > 2 else os.path.join(TOP, "Extract GameFiles")
GAME = sys.argv[2] if len(sys.argv) > 2 else os.path.join(TOP, "Game Files")
FILE = os.path.join("pac", "menu", "menu.pac")

MFLO = 0x25F000          # MFLO/0000 in menu.pac
MFLO_SLOT = 0x6800       # its directory slot
REC = 0x74
FIRST = MFLO + 0x28
# Entries added: (after the entry with this label, in this group) -> new label.
# The new record is a copy of that entry, which becomes not-last-of-group.
# Label ids are unused string ids; keep in sync with src/menu_hooks.cpp.
ADDITIONS = [
    # MY WWE -> OPTIONS: GRAPHICS after CHEAT CODES (a copy: screen 0x524).
    (0xA050, 0x11, 0xAFC0),
    # Main menu: EXIT after SHOP.
    (0xA04B, 0x01, 0xAFC1),
]


def u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def add_entry(data, after_label, group, new_label):
    total = u32(data, MFLO + 4)
    anchor = None
    for i in range(total):
        o = FIRST + i * REC
        if u32(data, o) == after_label and u32(data, o + 0x18) == group:
            anchor = o
            break
    if anchor is None or not (u32(data, anchor + 0x3C) & 2):
        raise SystemExit(f"{FILE}: entry {after_label:#x} in group {group:#x} not found / not last")
    end = FIRST + total * REC
    if end + REC > MFLO + MFLO_SLOT:
        raise SystemExit(f"{FILE}: no room in MFLO/0000")
    new = bytearray(data[anchor:anchor + REC])
    struct.pack_into(">I", new, 0x00, new_label)
    struct.pack_into(">I", new, 0x04, 0x05F5E0FF)            # no description text
    struct.pack_into(">I", new, 0x40, 0)                     # no "NEW" badge
    struct.pack_into(">I", data, anchor + 0x3C, u32(data, anchor + 0x3C) & ~2)  # not last
    at = anchor + REC
    data[at:end + REC] = new + data[at:end]                   # shift the rest down
    struct.pack_into(">I", data, MFLO + 4, total + 1)
    struct.pack_into(">I", data, MFLO + 8, u32(data, MFLO + 8) + 1)


def drop_skipped(data, label, group):
    """Removes a record the parser always skips (+0x40 u16 bit 0), making room
    in MFLO/0000's slot. The skipped records aren't counted in header +8, only
    in the total (+4). The record must not end its group (flag 2 resets the
    parser's row counter even on skipped records)."""
    total = u32(data, MFLO + 4)
    for i in range(total):
        o = FIRST + i * REC
        if u32(data, o) == label and u32(data, o + 0x18) == group:
            if not (data[o + 0x41] & 1) or u32(data, o + 0x3C) & 2:
                continue
            end = FIRST + total * REC
            data[o:end] = data[o + REC:end] + bytes(REC)
            struct.pack_into(">I", data, MFLO + 4, total - 1)
            return
    raise SystemExit(f"{FILE}: skipped entry {label:#x} in group {group:#x} not found")


def main() -> int:
    src = os.path.join(DISC, FILE)
    dst = os.path.join(GAME, FILE)
    data = bytearray(open(src, "rb").read())
    if (u32(data, MFLO + 4), u32(data, MFLO + 8)) != (0xE4, 0xDC):
        print(f"{FILE}: unexpected MFLO header; not patched")
        return 1
    # The table's slot has room for one more record. The game finds it by
    # its original place (moving it and its directory entry doesn't work), so
    # drop the main menu's hidden second ONLINE record (never shown) instead.
    drop_skipped(data, 0xA02E, 0x01)
    if 0x28 + u32(data, MFLO + 4) * REC + len(ADDITIONS) * REC > MFLO_SLOT:
        raise SystemExit(f"{FILE}: no room in MFLO/0000")
    for after_label, group, new_label in ADDITIONS:
        add_entry(data, after_label, group, new_label)
    if os.path.exists(dst) and open(dst, "rb").read() == data:
        print(f"{FILE}: already patched")
        return 0
    tmp = dst + ".tmp"
    open(tmp, "wb").write(data)
    os.replace(tmp, dst)
    print(f"{FILE}: added GRAPHICS (MY WWE -> OPTIONS) and EXIT (main menu)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
