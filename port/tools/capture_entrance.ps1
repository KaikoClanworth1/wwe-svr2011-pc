# Background check of match entrances (muted, off-screen): title -> practice
# arena -> main menu -> Play -> One on One -> Normal Match -> John Cena vs.
# Randy Orton -> Play, then a screenshot every 2 s to runs\<Name>NN.png, a
# contact sheet runs\<Name>_sheet.png, and the log's frame rates over the
# entrances.
#   .\capture_entrance.ps1 [-Name ent] [-Shots 45] [-Extra --unlock_30fps=false] [-GameDir <install>]
#                          [-P1 "RIGHT RIGHT"] [-P2 "LEFT"]
# -P1 / -P2: cursor moves on character select before each pick (player 1's
# cursor starts on John Cena, top row second from the right, and wraps;
# the computer's on Randy Orton): -P1 "RIGHT RIGHT" Batista,
# "RIGHT RIGHT DOWN RIGHT" Kane; "none": no moves.
param([string]$Name = "ent", [int]$Shots = 45, [string[]]$Extra = @(), [double]$Interval = 2, [string]$GameDir = "",
      [string]$P1 = "", [string]$P2 = "")
$s = Join-Path $PSScriptRoot "session.ps1"
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
if ($GameDir) { & $s start -Name $Name -GameDir $GameDir @Extra | Out-Null } else { & $s start -Name $Name @Extra | Out-Null }
function Step([string[]]$keys, [int]$sec) { & $s input @keys | Out-Null; Start-Sleep $sec }
Start-Sleep 50                          # boot (longer with DLC installed)
Step @("press START 200") 40            # title -> practice arena
Step @("press A 200") 5                 # dismiss a notice (e.g. DLC unlocks), if any
Step @("press START 200") 6             # main menu
Step @("press A 200") 4                 # Play
Step @("press A 200") 6                 # One on One
Step @("press A 200") 10                # Normal Match -> character select
Step @("press A 200") 3                 # player 1 joins (the cursor appears on John Cena)
foreach ($k in ($P1 -split "\s+" | Where-Object { $_ -and $_ -ne "none" })) { Step @("press $k 200") 2 }
& $s shot "${Name}_p1" | Out-Null       # (player 1's pick)
Step @("press A 200") 4                 # superstar 1
Step @("press A 200") 4                 # ready (the computer's cursor appears on Randy Orton)
foreach ($k in ($P2 -split "\s+" | Where-Object { $_ -and $_ -ne "none" })) { Step @("press $k 200") 2 }
& $s shot "${Name}_p2" | Out-Null       # (player 2's pick)
Step @("press A 200") 4                 # superstar 2
Step @("press A 200") 16                # ready -> versus screen
# A press can be lost in a transition: confirm again until the versus
# screen shows.
$check = Join-Path $runs "${Name}00.png"
for ($t = 0; ; $t++) {
    & $s shot "${Name}00" | Out-Null
    # Versus screen: its button row is grey on black (character select has
    # the colourful roster there).
    $red = python -c "import sys; from PIL import Image, ImageStat; h,s,v=ImageStat.Stat(Image.open(sys.argv[1]).convert('HSV').resize((1280,748)).crop((400,560,630,584))).mean; print(1 if s<40 else 0)" $check
    if ($red -eq "1") { break }
    if ($t -ge 2) { Write-Output "versus screen not recognised - going on (see $check)"; break }
    Step @("press A 200") 16
}
$started = Get-Date
Step @("press A 200") 0                 # Play
for ($k = 1; $k -le $Shots; $k++) { Start-Sleep $Interval; & $s shot ("{0}{1:D2}" -f $Name, $k) | Out-Null }
& $s stop | Out-Null
python -c "
import glob,sys
from PIL import Image, ImageStat
fs=sorted(glob.glob(sys.argv[1]+'[0-9][0-9].png'))
print(' '.join('%s:%d'%(f[-6:-4],ImageStat.Stat(Image.open(f).convert('L')).mean[0]) for f in fs))
ims=[Image.open(f).convert('RGB').resize((320,187)) for f in fs]
out=Image.new('RGB',(1600,187*((len(ims)+4)//5)))
for i,im in enumerate(ims): out.paste(im,((i%5)*320,(i//5)*187))
out.save(sys.argv[1]+'_sheet.png'); print(sys.argv[1]+'_sheet.png')" (Join-Path $runs $Name)
Get-Content (Join-Path $runs "$Name.log") | Select-String "fps:" |
    Where-Object { [datetime]::ParseExact($_.Line.Substring(1, 23), "yyyy-MM-dd HH:mm:ss.fff", $null) -gt $started } |
    ForEach-Object { $_.Line.Substring(12, 60) }
