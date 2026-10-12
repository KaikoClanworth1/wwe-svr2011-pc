#!/bin/sh
# HCTP Arena Back Entrance (stage 0x30) -> SvR2011 backstage row mod in $H/mod_row
set -e
H=$(dirname "$0"); cd "$H"
G="D:/Xbox Games Ports/WWE Smackdown Vs Raw 2011"
T="$G/port/tools"
STG="D:/Xbox Games Ports/HCTP Extract/_unpacked/bg/stg_0030"
FAKE_OPTS=${FAKE_OPTS:---climb-df}
python hctp_fake56.py "$STG" "$G/Extract GameFiles/pac/bg/bg78.pac" out/fake56_30.pac --report $FAKE_OPTS > out/fake56_30.log
python stage_wrap.py out/fake56_30.pac "$G/Extract GameFiles/pac/bg/bg78.pac" out/bg78_hctp30.pac \
  --rotate 180 --offset 180,-440 --drop-2011-cars --report > out/bg78_hctp30.log
# the real stage first in STG (RedirectArena renames the first entry when it isn't the slot's name)
python -c "
import sys; sys.path.insert(0, r'$T')
import svrfmt as f
h, g, t = f.epac_read(open('out/bg78_hctp30.pac', 'rb').read())
ty, e = g[0]
# parking ids the HCTP room leaves empty: stub models (the game looks some of them up by id)
import svr08_stage as s8
_, _, _, _, e78 = s8.stage(r'$G/Extract GameFiles/pac/bg/bg78.pac'); d78 = dict(e78)
k = [i for i, (n, b) in enumerate(e) if n == b'0078'][0]
ents = f.pach_read(e[k][1]); have = {i for i, b in ents}
ents += [(i, f.bpe_pack(s8.stub_model(s8.unpack(d78[i])))) for i in range(20) if i not in have and i in d78]
ents.sort(key=lambda x: x[0])
e[k] = (b'0078', f.pach_write(ents))
print('stubs', sorted(i for i in range(20) if i not in have and i in d78))
open('mod_row/arena.pac', 'wb').write(f.epac_write(h, [(ty, e)], t))"
python build_props.py | tail -1
cp mod_row/gimmick.pac mod_row/gimmick.pac.bak 2>/dev/null; true
tail -3 out/bg78_hctp30.log
# the manifest: own row after the parking lot, HCTP room logic on, the room's
# loose weapons (HCTP 50000 'w' + GmWeaponPosRot 230/231; 2011 trash can 63)
printf 'type=backstage\r\nid=hctp_back_entrance\r\nname=Arena Back Entrance (HCTP)\r\nauthor=HCTP port\r\nversion=0.1\r\narea=0\r\nrow=Back Entrance (HCTP)\r\ngimmick=78H3\r\nbox=181.5,-478.4,94,66\r\ncamera_height=45\r\nhctp=30\r\nweapons=63,273.3,-30.3,-538.2,0;63,92,0,-453,0;63,223,0,-453,0\r\nload_weapons=17,88\r\n' > mod_row/manifest.txt
ls -la mod_row
