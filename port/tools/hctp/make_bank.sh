#!/bin/sh
# room 30's gimmick bank: 2011 78PK bank + HCTP motions (keys 12501-12558)
set -e
cd "$(dirname "$0")"
HB="D:/Xbox Games Ports/HCTP Extract/_unpacked/m/MOT_GAME"
JM=0=0,1=1,153=153      # jumps to motions 2011 has under the same number (1660 has none: dropped)
python mot/hctp2ymks.py "$HB/00000.bin" 256=12501,262=12502,263=12503,664=12504,340=12516 --into out/mot/78PK_bank.bin --out out/mot/b1.bin --jumpmap $JM | tail -1
python mot/hctp2ymks.py "$HB/00004.bin" 12572=12505,12092=12506,12045=12507,12046=12508,12047=12509,12035=12510,12036=12511,12037=12512,12034=12513,12043=12514,12014=12515,12060=12545 --into out/mot/b1.bin --out out/mot/b2.bin --jumpmap $JM | tail -1
python mot/hctp2ymks.py "D:/Xbox Games Ports/HCTP Extract/_unpacked/bg/stg_0030/50102.bin" 23=12523,24=12524,25=12525,26=12526,27=12527,28=12528,29=12529,30=12530,31=12531,32=12532,33=12533 --objects --into out/mot/b2.bin --out out/mot/b3.bin | tail -1
python - <<'PY'
import bank_rekey
d = open('out/mot/b3.bin', 'rb').read()
m = {(12501, 1): (12541, 0), (12501, 2): (12542, 0), (12502, 2): (12544, 0), (12502, 1): (12543, 0),
     (12528, 0): (12558, 0), (12526, 0): (12556, 0), (12525, 0): (12555, 0), (12524, 0): (12554, 0)}
fr = {(12558, 0): 1, (12556, 0): 1, (12555, 0): 1, (12554, 0): 1}
d = bank_rekey.rekey(d, m, fr)
# the equipment crate's rest pose: its own track (y 2) of the shut-in act, 1 frame
d = bank_rekey.copy_track(d, (12513, 0, 2), (12559, 0, 0), frames=1)
open('out/mot/bank_h3.bin', 'wb').write(d)
print('bank_h3.bin written')
PY
