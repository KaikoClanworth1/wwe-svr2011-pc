"""Packs the native renderer's shaders into one file per graphics API.

  python tools/pack_shaders.py <native_shaders folder> [--remove-loose]

Writes shaders.dxil.pak (D3D12), shaders.spv.pak (Vulkan) and shaders.spvc.pak
(Vulkan GPUs without descriptor indexing: compact tables) into the folder:
each holds that API's shader code (*.dxil / *.spv, without debug_* / dbg_*)
and the shared *.inputs / *.textures. The game reads the pack in one go
instead of opening a thousand small files (each a seek on a hard disk).
--remove-loose then deletes the packed loose files (the release zip ships
the packs only). Format (little-endian): b"SVRPAK1\\0", u32 count, per file
u16 name length + name + u32 offset + u32 size, then the files' bytes.
"""
import struct
import sys
from pathlib import Path


def pack(folder, ext):
    files = sorted(f for f in folder.iterdir()
                   if f.is_file() and not f.name.startswith(("dbg_", "debug_"))
                   and (f.suffix == ext or f.suffix in (".inputs", ".textures")))
    if not any(f.suffix == ext for f in files):
        return None, []
    names = [f.name.encode("utf-8") for f in files]
    header = 12 + sum(2 + len(n) + 8 for n in names)
    index, blobs, offset = b"", [], header
    for f, n in zip(files, names):
        data = f.read_bytes()
        index += struct.pack("<H", len(n)) + n + struct.pack("<II", offset, len(data))
        blobs.append(data)
        offset += len(data)
    out = folder / f"shaders{ext}.pak"
    tmp = out.with_suffix(".tmp")
    with open(tmp, "wb") as w:
        w.write(b"SVRPAK1\0" + struct.pack("<I", len(files)) + index)
        for b in blobs:
            w.write(b)
    tmp.replace(out)
    print(f"{out.name}: {len(files)} files, {offset // 1024} KB")
    return out, files


def main(args):
    if not args:
        print(__doc__)
        return 1
    folder = Path(args[0])
    packed = set()
    for ext in (".dxil", ".spv", ".spvc"):
        out, files = pack(folder, ext)
        if out:
            packed.update(files)
    if "--remove-loose" in args:
        for f in packed:
            f.unlink()
        print(f"removed {len(packed)} loose files")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
