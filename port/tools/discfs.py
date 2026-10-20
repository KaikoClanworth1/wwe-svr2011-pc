"""Read game files straight from a disc image - no extracting.

One way in for every converter: open() takes an extracted folder, an Xbox 360
(or original Xbox) disc image (XDVDFS) or a PS2 disc image (ISO 9660), and
gives the same small interface for all three:

    import discfs
    with discfs.open(r'D:\\Roms\\Xbox 360\\WWE\\WWE SmackDown vs. Raw 2008.iso') as d:
        d.kind                       # 'folder' | 'xdvdfs' | 'iso9660'
        d.listdir('pac/ch')          # names in a folder ('' = the root)
        d.exists('pac/ch/ch100.pac') # paths: '/' or '\\', any case
        d.size('pac/ch/ch100.pac')
        data = d.read('pac/ch/ch100.pac')            # the whole file
        head = d.read('pac/ch/ch100.pac', 0, 0x800)  # or a part of it
        for path, size in d.walk('pac'): ...         # every file under a folder
        d.info()                     # {kind, files, title_id | serial, ...}

Paths are matched without regard to case (PS2 names are upper case, with
a ';1' version the reader drops). Directory tables are read once at open;
file data is read only when asked for.

As a command (JSON on stdout, for the Mod Maker and for checks):
    py discfs.py info <image or folder>
    py discfs.py ls <image or folder> [folder]
    py discfs.py cat <image or folder> <file> > out.bin
    py discfs.py get <image or folder> <file or folder> <out folder>

Formats: XDVDFS - the volume descriptor ("MICROSOFT*XBOX*MEDIA") at sector 32
of the game partition, whose offset depends on the disc format (plain XISO,
XGD1, XGD2, XGD3; see extract_iso.py); directory tables are binary trees of
14-byte entries + name, 4-byte aligned, 0xFFFF padding to the sector end.
ISO 9660 - the primary volume descriptor at sector 16, directory records
(length, extent, size, flags, name).
"""
import builtins
import json
import os
import struct
import sys

SECTOR = 2048
XBOX_MAGIC = b"MICROSOFT*XBOX*MEDIA"
XBOX_OFFSETS = (0, 0x18300000, 0xFD90000, 0x2080000)  # XISO, XGD1, XGD2, XGD3


def _norm(path):
    p = path.replace("\\", "/").strip("/")
    return "/".join(x for x in p.split("/") if x and x != ".").lower()


class _Image:
    """A disc image: an index {lower path: (offset, size, is_dir, name)}."""

    def __init__(self, path, kind):
        self.path = path
        self.kind = kind
        self.f = builtins.open(path, "rb")
        self.index = {"": (0, 0, True, "")}
        self.children = {"": []}

    def _add(self, parent, name, offset, size, is_dir):
        key = (parent + "/" + name).lower() if parent else name.lower()
        if key in self.index:  # (a duplicate entry: keep the first)
            return key
        self.index[key] = (offset, size, is_dir, name)
        self.children.setdefault(parent, []).append(key)
        if is_dir:
            self.children.setdefault(key, [])
        return key

    def _entry(self, path):
        e = self.index.get(_norm(path))
        if e is None:
            raise FileNotFoundError(path)
        return e

    def exists(self, path):
        return _norm(path) in self.index

    def isdir(self, path):
        e = self.index.get(_norm(path))
        return bool(e and e[2])

    def size(self, path):
        return self._entry(path)[1]

    def listdir(self, path=""):
        key = _norm(path)
        if key not in self.children:
            raise NotADirectoryError(path)
        return [self.index[k][3] for k in self.children[key]]

    def walk(self, path=""):
        """(path with its real case, size) of every file under a folder."""
        start = _norm(path)
        stack = [start]
        while stack:
            d = stack.pop()
            for k in self.children.get(d, []):
                off, size, is_dir, _ = self.index[k]
                if is_dir:
                    stack.append(k)
                else:
                    yield self.real_path(k), size

    def real_path(self, key):
        parts, cur = [], key
        while cur:
            parts.append(self.index[cur][3])
            cur = cur.rpartition("/")[0]
        return "/".join(reversed(parts))

    def read(self, path, offset=0, length=None):
        off, size, is_dir, _ = self._entry(path)
        if is_dir:
            raise IsADirectoryError(path)
        offset = max(0, min(offset, size))
        n = size - offset if length is None else max(0, min(length, size - offset))
        self.f.seek(off + offset)
        return self.f.read(n)

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


class Xdvdfs(_Image):
    def __init__(self, path):
        super().__init__(path, "xdvdfs")
        f = self.f
        self.partition = None
        for off in XBOX_OFFSETS:
            f.seek(off + 32 * SECTOR)
            if f.read(len(XBOX_MAGIC)) == XBOX_MAGIC:
                self.partition = off
                break
        if self.partition is None:
            raise ValueError("no XDVDFS volume (not an Xbox disc image)")
        root_sector, root_size = struct.unpack("<II", f.read(8))
        self._dir(root_sector, root_size, "", 0)

    def _dir(self, sector, size, parent, depth):
        if size == 0 or depth > 32:
            return
        self.f.seek(self.partition + sector * SECTOR)
        table = self.f.read(size)
        pos = 0
        while pos + 14 <= len(table):
            if struct.unpack_from("<H", table, pos)[0] == 0xFFFF:  # padding to the sector end
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            _, _, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, pos)
            name = table[pos + 14:pos + 14 + nlen].decode("latin-1")
            is_dir = bool(attr & 0x10)
            key = self._add(parent, name, self.partition + start * SECTOR, length, is_dir)
            if is_dir:
                self._dir(start, length, key, depth + 1)
            pos = (pos + 14 + nlen + 3) & ~3

    def info(self):
        out = {"kind": self.kind, "partition": hex(self.partition), "files": sum(1 for k, e in self.index.items() if not e[2])}
        if self.exists("default.xex"):
            out.update(_xex_info(self.read("default.xex", 0, 0x10000)))
        return out


class Iso9660(_Image):
    def __init__(self, path):
        super().__init__(path, "iso9660")
        f = self.f
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        if pvd[1:6] != b"CD001" or pvd[0] != 1:
            raise ValueError("no ISO 9660 volume")
        self.volume = pvd[40:72].decode("latin-1").strip()
        root = pvd[156:190]
        self._dir(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "", 0)

    def _dir(self, lba, size, parent, depth):
        if depth > 32:
            return
        self.f.seek(lba * SECTOR)
        d = self.f.read(size)
        p = 0
        while p < len(d):
            n = d[p]
            if n == 0:  # records don't cross sectors
                p = (p // SECTOR + 1) * SECTOR
                continue
            ext = struct.unpack_from("<I", d, p + 2)[0]
            ln = struct.unpack_from("<I", d, p + 10)[0]
            flags = d[p + 25]
            nl = d[p + 32]
            raw = d[p + 33:p + 33 + nl]
            if raw not in (b"\0", b"\1"):
                name = raw.decode("latin-1").split(";")[0]
                if name.endswith(".") and not flags & 2:
                    name = name[:-1]
                is_dir = bool(flags & 2)
                key = self._add(parent, name, ext * SECTOR, ln, is_dir)
                if is_dir:
                    self._dir(ext, ln, key, depth + 1)
            p += n

    def info(self):
        out = {"kind": self.kind, "volume": self.volume, "files": sum(1 for k, e in self.index.items() if not e[2])}
        out.update(_cnf_info(self))
        return out


class Folder:
    """An extracted disc (or any folder): the same interface, any case."""

    def __init__(self, path):
        self.path = os.path.abspath(path)
        self.kind = "folder"

    def _real(self, path):
        cur = self.path
        for part in [x for x in path.replace("\\", "/").split("/") if x and x != "."]:
            nxt = os.path.join(cur, part)
            if not os.path.exists(nxt):
                low = part.lower()
                try:
                    match = next((n for n in os.listdir(cur) if n.lower() == low), None)
                except OSError:
                    match = None
                if match is None:
                    raise FileNotFoundError(path)
                nxt = os.path.join(cur, match)
            cur = nxt
        return cur

    def exists(self, path):
        try:
            self._real(path)
            return True
        except FileNotFoundError:
            return False

    def isdir(self, path):
        try:
            return os.path.isdir(self._real(path))
        except FileNotFoundError:
            return False

    def size(self, path):
        return os.path.getsize(self._real(path))

    def listdir(self, path=""):
        return sorted(os.listdir(self._real(path)))

    def walk(self, path=""):
        base = self._real(path)
        for root, _, files in os.walk(base):
            for n in files:
                full = os.path.join(root, n)
                yield os.path.relpath(full, self.path).replace("\\", "/"), os.path.getsize(full)

    def read(self, path, offset=0, length=None):
        with builtins.open(self._real(path), "rb") as f:
            f.seek(offset)
            return f.read() if length is None else f.read(length)

    def info(self):
        out = {"kind": self.kind, "files": sum(1 for _ in self.walk())}
        if self.exists("default.xex"):
            out.update(_xex_info(self.read("default.xex", 0, 0x10000)))
        out.update(_cnf_info(self))   # (an extracted PS2 disc: its serial, as from the image)
        return out

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        pass


def _cnf_info(d):
    """A PS2 disc's serial from its SYSTEM.CNF (BOOT2 = cdrom0:\\SLUS_123.45;1)."""
    out = {}
    if d.exists("SYSTEM.CNF"):
        for line in d.read("SYSTEM.CNF").decode("latin-1").splitlines():
            if line.replace(" ", "").upper().startswith("BOOT2="):
                boot = line.split("=", 1)[1].strip()
                out["serial"] = boot.replace("cdrom0:", "").strip("\\/").split(";")[0].upper()  # (as the Mod Maker reads it)
    return out


def _xex_info(head):
    """Title id / media id from an XEX2 header's execution info (key 0x40006)."""
    out = {}
    if head[:4] != b"XEX2":
        return out
    count = struct.unpack_from(">I", head, 0x14)[0]
    for i in range(min(count, 64)):
        key, val = struct.unpack_from(">II", head, 0x18 + i * 8)
        if key == 0x00040006 and val + 16 <= len(head):
            media, version, base, title = struct.unpack_from(">IIII", head, val)
            out["title_id"] = "%08X" % title
            out["media_id"] = "%08X" % media
            out["version"] = "%d.%d.%d.%d" % (version >> 28, (version >> 24) & 15, (version >> 8) & 0xFFFF, version & 255)
    return out


def open(path):  # noqa: A001 - discfs.open is the point
    """A folder, an Xbox (XDVDFS) disc image or a PS2 (ISO 9660) disc image."""
    if os.path.isdir(path):
        return Folder(path)
    with builtins.open(path, "rb") as f:
        f.seek(16 * SECTOR)
        pvd = f.read(8)
        xbox = False
        for off in XBOX_OFFSETS:
            f.seek(off + 32 * SECTOR)
            if f.read(len(XBOX_MAGIC)) == XBOX_MAGIC:
                xbox = True
                break
    if xbox:  # (360 discs also carry an ISO 9660 stub: XDVDFS first)
        return Xdvdfs(path)
    if pvd[1:6] == b"CD001":
        return Iso9660(path)
    raise ValueError("not a folder, Xbox disc image or ISO 9660 image: %s" % path)


def _main(argv):
    if len(argv) < 3 or argv[1] not in ("info", "ls", "cat", "get"):
        raise SystemExit(__doc__)
    try:
        d = open(argv[2])
    except (OSError, ValueError) as e:
        print(json.dumps({"ok": False, "error": str(e)}))
        return 2
    with d:
        if len(argv) > 3 and not d.exists(argv[3]):
            print(json.dumps({"ok": False, "error": "not on the disc: " + argv[3]}))
            return 2
        if argv[1] == "info":
            print(json.dumps(d.info()))
        elif argv[1] == "ls":
            p = argv[3] if len(argv) > 3 else ""
            print(json.dumps([{"name": n, "dir": d.isdir(p + "/" + n if p else n),
                               "size": 0 if d.isdir(p + "/" + n if p else n) else d.size(p + "/" + n if p else n)}
                              for n in d.listdir(p)]))
        elif argv[1] == "cat":
            sys.stdout.buffer.write(d.read(argv[3]))
        else:
            src, out = argv[3], argv[4]
            files = list(d.walk(src)) if d.isdir(src) else [(src, d.size(src))]
            base = src.replace("\\", "/").strip("/").rpartition("/")[0] if not d.isdir(src) else src.replace("\\", "/").strip("/")
            for i, (p, size) in enumerate(files):
                rel = p[len(base):].lstrip("/") if base and p.lower().startswith(base.lower()) else p
                dst = os.path.join(out, rel)
                os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
                with builtins.open(dst, "wb") as o:
                    o.write(d.read(p))
                print("progress %d %s" % (100 * (i + 1) // len(files), rel), file=sys.stderr)
            print(json.dumps({"ok": True, "files": len(files)}))
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
