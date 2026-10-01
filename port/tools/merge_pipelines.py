"""Merges native-renderer pipeline lists into the shipped one.

The native renderer notes every pipeline it builds in
<user data>\\cache\\native_pipelines.list (one line each: the key, the slot
strides and the vertex layout). The release ships port/dist/pipelines.list
(copied to native_shaders\\ by deploy.ps1 / package.ps1); the game builds those
in the background in the menus, so scenes don't stutter compiling them.

  python tools/merge_pipelines.py <list or folder> ...   (folders: every
      native_pipelines.list under them, e.g. runs\\crawl)

New lines are appended after the existing ones (the game builds them in this
order, so the menus' come first). Lines whose shaders aren't in
runs/shaders_native/dxil (a shader set from an older conversion) are dropped.
"""
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
SHIPPED = PORT / "dist" / "pipelines.list"
SHADERS = PORT / "runs" / "shaders_native" / "dxil"


def lines_of(path):
    return [l.strip() for l in path.read_text(encoding="utf-8", errors="replace").splitlines() if "|" in l]


def shaders_exist(line):
    f = line.split()
    vs, ps = f[0], f[2]
    if not (SHADERS / f"{vs}.vs.dxil").exists():
        return False
    return ps == "0000000000000000" or (SHADERS / f"{ps}.ps.dxil").exists()


def main(args):
    if not args:
        print(__doc__)
        return 1
    merged = lines_of(SHIPPED) if SHIPPED.exists() else []
    seen = set(merged)
    before = len(merged)
    dropped = 0
    for a in args:
        p = Path(a)
        files = sorted(p.rglob("native_pipelines.list")) if p.is_dir() else [p]
        for f in files:
            for line in lines_of(f):
                if line in seen:
                    continue
                seen.add(line)
                if SHADERS.exists() and not shaders_exist(line):
                    dropped += 1
                    continue
                merged.append(line)
    SHIPPED.parent.mkdir(parents=True, exist_ok=True)
    SHIPPED.write_text("\n".join(merged) + "\n", encoding="utf-8", newline="\n")
    print(f"{SHIPPED}: {len(merged)} pipelines ({len(merged) - before} new, {dropped} dropped: shaders missing)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
