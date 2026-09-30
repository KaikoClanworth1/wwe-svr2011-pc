"""Source lines of a simpleperf address report (phone profiles).

  simpleperf report -i perf.data --symdir <dir> --sort dso,vaddr_in_file -n > addrs.txt
  python perf_lines.py addrs.txt <unstripped lib> [top]

Sums the samples per function and per source line of that library, using the
NDK's llvm-symbolizer on the unstripped build (out/build/Android/libmain.so).
"""
import collections
import os
import re
import subprocess
import sys

report, lib = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
name = os.path.basename(lib)
symbolizer = os.path.join(os.environ.get("ANDROID_HOME", r"D:\Android\sdk"), "ndk", "30.0.16248370",
                          "toolchains", "llvm", "prebuilt", "windows-x86_64", "bin", "llvm-symbolizer.exe")
rows = []
for line in open(report, encoding="utf-8-sig", errors="replace"):
    m = re.match(r"\s*([\d.]+)%\s+(\d+)\s+(\S*" + re.escape(name) + r")\s+(0x[0-9a-f]+)", line)
    if m:
        rows.append((float(m.group(1)), m.group(4)))
out = subprocess.run([symbolizer, "--obj=" + lib, "--inlining=false", "--functions=short"] + [a for _, a in rows],
                     capture_output=True, text=True).stdout.strip().split("\n\n")
by_line, by_fn = collections.Counter(), collections.Counter()
for (pct, _), block in zip(rows, out):
    lines = block.strip().split("\n")
    fn = lines[0] if lines else "?"
    src = lines[1].replace("\\", "/") if len(lines) > 1 else "?"
    src = re.sub(r".*/port/src/", "", src)
    src = re.sub(r":\d+$", "", src)  # (column)
    by_line[(fn, src)] += pct
    by_fn[fn] += pct
print(f"-- by function ({name}, % of the profiled samples)")
for fn, p in by_fn.most_common(12):
    print(f"{p:5.2f}%  {fn}")
print("-- by line")
for (fn, src), p in by_line.most_common(top):
    print(f"{p:5.2f}%  {src}  [{fn}]")
