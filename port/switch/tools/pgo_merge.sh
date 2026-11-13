#!/bin/bash
# Profile-guided optimisation, step 2: the sessions a counting build wrote on the card (SvR2011/pgo/*.stream, see
# switch/nx_pgo.cpp) become the profile a build with NX_PGO=use reads.
#   pgo_merge.sh <folder with the .stream files>
# Run from MSYS2's bash, from the drive and on the PC the counting build was made on: each stream names its files
# by that build's profile folder (NX_PGO_DIR). Sessions are added together, so the profile folder must be empty
# first (running the same streams twice would count them twice).
set -e
IN="$1"
PGO="${NX_PGO_DIR:-${NX_WORK:-/work/nx}/pgo}"
[ -d "$IN" ] || { echo "pgo_merge.sh <folder with the .stream files>"; exit 1; }
mkdir -p "$PGO"
[ -z "$(find "$PGO" -name '*.gcda' | head -1)" ] || { echo "$PGO already has a profile: move it aside first"; exit 1; }
export PATH=/opt/devkitpro/devkitA64/bin:$PATH
# (as a file, not through stdin: Windows would read stdin as text)
n=0
for f in "$IN"/*.stream; do
  [ -f "$f" ] || continue
  aarch64-none-elf-gcov-tool merge-stream "$(cygpath -m "$f")"
  n=$((n + 1))
done
# The compiler names each object's file by the folder it was built in, as Windows sees it (C~/msys64/work/...), under
# the profile folder: the counting build's. They move to the name the build that reads them looks for.
USE="$PGO/$(cygpath -m "${NX_BUILD:-${NX_WORK:-/work/nx}/b}" | sed 's/:/~/')"
mkdir -p "$USE"
find "$PGO" -name '*.gcda' -not -path "$USE/*" -exec mv {} "$USE/" \;
find "$PGO" -depth -type d -empty -delete
python "$(dirname "${BASH_SOURCE[0]}")/pgo_summary.py" "$USE" "$n"
echo "pgo: $n sessions, $(ls "$USE"/*.gcda 2> /dev/null | wc -l) objects in $USE"
