#!/bin/sh
# Any HCTP backstage room -> SvR2011 backstage row mod (run in a work folder holding out/mot/bank_h3.bin)
#   sh build_room.sh <room 30..36> <dx> <dz> <mod folder> <row name>
# (half turn + offset: put the room's centre on the parking fight-box centre (125, -410))
set -e
ROOM=$1; DX=$2; DZ=$3; MOD=$4; NAME=$5
S=$(cd "$(dirname "$0")" && pwd)
G="D:/Xbox Games Ports/WWE Smackdown Vs Raw 2011"
T="$G/port/tools"
STG="D:/Xbox Games Ports/HCTP Extract/_unpacked/bg/stg_00$ROOM"
mkdir -p out "$MOD"
python "$S/hctp_fake56.py" "$STG" "$G/Extract GameFiles/pac/bg/bg78.pac" out/fake56_$ROOM.pac --report --climb-df > out/fake56_$ROOM.log
python "$S/stage_wrap.py" out/fake56_$ROOM.pac "$G/Extract GameFiles/pac/bg/bg78.pac" out/bg78_hctp$ROOM.pac \
  --rotate 180 --offset $DX,$DZ --drop-2011-cars --report > out/bg78_hctp$ROOM.log
# the real stage first in STG; empty parking ids get stub models (the game looks some of them up by id)
python -c "
import sys; sys.path.insert(0, r'$T')
import svrfmt as f
import svr08_stage as s8
h, g, t = f.epac_read(open('out/bg78_hctp$ROOM.pac', 'rb').read())
ty, e = g[0]
_, _, _, _, e78 = s8.stage(r'$G/Extract GameFiles/pac/bg/bg78.pac'); d78 = dict(e78)
k = [i for i, (n, b) in enumerate(e) if n == b'0078'][0]
ents = f.pach_read(e[k][1]); have = {i for i, b in ents}
ents += [(i, f.bpe_pack(s8.stub_model(s8.unpack(d78[i])))) for i in range(20) if i not in have and i in d78]
ents.sort(key=lambda x: x[0])
e[k] = (b'0078', f.pach_write(ents))
print('stubs', sorted(i for i in range(20) if i not in have and i in d78))
open('$MOD/arena.pac', 'wb').write(f.epac_write(h, [(ty, e)], t))"
HCTP_ROOM=$ROOM HCTP_DX=$DX HCTP_DZ=$DZ HCTP_OUT="$MOD" python "$S/build_props.py" | tail -1
tail -3 out/bg78_hctp$ROOM.log
# the fight box: the room's extent (cx, cz, half x, half z) from the stage log
BOX=$(python -c "
import re
m = re.search(r'x (-?[\d.]+)\.\.(-?[\d.]+), z (-?[\d.]+)\.\.(-?[\d.]+)\)', open('out/bg78_hctp$ROOM.log').read())
x0, x1, z0, z1 = map(float, m.groups())
print('%.1f,%.1f,%.1f,%.1f' % ((x0 + x1) / 2, (z0 + z1) / 2, (x1 - x0) / 2, (z1 - z0) / 2))")
printf 'type=backstage\r\nid=hctp_room_%s\r\nname=%s\r\nauthor=HCTP port\r\nversion=0.1\r\narea=0\r\nrow=%s\r\ngimmick=78H3\r\nbox=%s\r\ncamera_height=45\r\nhctp=%s\r\n' \
  "$ROOM" "$NAME" "$NAME" "$BOX" "$ROOM" > "$MOD/manifest.txt"
ls -la "$MOD"
