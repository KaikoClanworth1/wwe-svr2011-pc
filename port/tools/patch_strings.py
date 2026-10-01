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
LABELS = {b"Xbox LIVE": b"ONLINE", b"GAMERTAG": b"NAME"}
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
    # Online Axxess: none needed on the PC (all content is licensed)
    (b' This item is free with redemption of the single-use Online Axxess code found on the back of your game manual. If the code has already been redeemed by a previous owner, then you can purchase an Online Axxess code in the store.',
     b''),
    (b'Check out the store to redeem your\nONLINE Axxess code, and purchase\ndownloadable content',
     b'Check out the store for\ndownloadable content'),
    (b'From now on everything besides\nONLINE AXXESS downloadable content is free!',
     b'From now on all downloadable\ncontent is free!'),
    (b' Please note: This purchase does not include Online Axxess or WWE Legend Bret Hart.',
     b' Please note: This purchase does not include WWE Legend Bret Hart.'),
    # (the same in the other languages)
    (b'usa la Xbox Dashboard per creare una playlist contenente solo la canzone desiderata.',
     b'mettila da sola in una cartella dentro la cartella Music del gioco.'),
    (b'erstelle \xc3\xbcber die Xbox Steuerung eine Wiedergabeliste, die nur diesen Song enth\xc3\xa4lt.',
     b'lege nur diesen Song in einen eigenen Ordner im Music-Ordner des Spiels.'),
    (b'crea una lista de reproducci\xc3\xb3n que solo contenga esa canci\xc3\xb3n con el Interfaz Xbox.',
     b'pon solo esa canci\xc3\xb3n en su propia carpeta dentro de la carpeta Music del juego.'),
    (b"cr\xc3\xa9ez une s\xc3\xa9lection ne contenant que cette musique\nvia l'Interface Xbox.",
     b'mettez-la seule dans un dossier\ndu dossier Music du jeu.'),
    (b'Xbox \xe3\x83\x80\xe3\x83\x83\xe3\x82\xb7\xe3\x83\xa5\xe3\x83\x9c\xe3\x83\xbc\xe3\x83\x89\xe3\x81\x8b\xe3\x82\x89\xe3\x80\x81\n\xe3\x81\x9d\xe3\x81\xae\xe6\x9b\xb2\xe3\x81\xa0\xe3\x81\x91\xe3\x81\x8c\xe5\x85\xa5\xe3\x81\xa3\xe3\x81\x9fPlaylist\xe3\x82\x92\xe4\xbd\x9c\xe6\x88\x90\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84\xe3\x80\x82',
     b'\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0\xe3\x81\xaeMusic\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe3\x81\xab\xe3\x80\x81\n\xe3\x81\x9d\xe3\x81\xae\xe6\x9b\xb2\xe3\x81\xa0\xe3\x81\x91\xe3\x81\xae\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80\xe3\x82\x92\xe4\xbd\x9c\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84\xe3\x80\x82'),
    (b"storage device", b"save folder"),
    # CAW logos: the port allows 10 High Resolution logos (src/caw_logos.cpp)
    (b"A maximum of 2 different High Resolution logos can be",
     b"Up to 10 different High Resolution logos can be"),
    # Xbox LIVE features the PC doesn't have (English text; the button help
    # for gamer cards and parties goes by string id, src/online.cpp)
    (b"\xee\x80\x95VIEW GAMER CARD   ", b""),
    (b"the Xbox LIVE Marketplace", b"the SHOP"),
    (b"the Online Marketplace", b"the SHOP"),
    (b"Xbox LIVEmenu", b"Online menu"),
    (b"Onlinemenu", b"Online menu"),  # (patched before as one word)
    (b"gamer profile", b"profile"),
    (b"Accessing Gamertag...", b"Accessing name..."),
    (b"customize your Gamertag display", b"customize your name display"),
    (b"CUSTOM SEARCH FROM GAMERTAG", b"CUSTOM SEARCH FROM NAME"),
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
            b"A maximum of 2 different High Resolution",
            b"VIEW GAMER CARD   ", b"Marketplace", b"Onlinemenu", b"gamer profile", b"Gamertag",
            b"GAMERTAG", b"Xbox", b"Online Axxess", b"ONLINE Axxess", b"ONLINE AXXESS"]


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
                # (a string may grow into the nulls a shorter patch left)
                room = end
                while room + 1 < len(data) and data[room + 1] == 0:
                    room += 1
                if new != old and start + len(new) <= room:
                    data[start:max(end, start + len(new) + 1)] = new + b"\x00" * (max(end, start + len(new) + 1) - start - len(new))
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
