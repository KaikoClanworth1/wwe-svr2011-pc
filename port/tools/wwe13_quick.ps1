# Quick look at an arena variant: the match test with a few shots, then a sheet
# runs\<Name>_sheet.png. Same rules as wwe13_match_test.ps1 (own game, waits).
#   wwe13_quick.ps1 -Pac <arena.pac> -Name <n> [-Shots 6] [-Every 3]
param([Parameter(Mandatory = $true)][string]$Pac, [Parameter(Mandatory = $true)][string]$Name, [int]$Shots = 6, [int]$Every = 3,
      [int]$Settle = 50, [switch]$NoWait)
$runs = "D:\Xbox Games Ports\SvR2011 Arenas\port\runs"
$env:SVR2011_TEST_RUN = "1"
& (Join-Path $PSScriptRoot "wwe13_match_test.ps1") -Pac $Pac -Name $Name -Shots $Shots -Every $Every -Settle $Settle `
    -People "JOHN CENA,RANDY ORTON,BATISTA" -Rule "0D" -Acts @("wait 100") -NoWait:$NoWait | Out-Null
Remove-Item Env:SVR2011_TEST_RUN
$py = @"
from PIL import Image
ims=[Image.open(r'$runs\${Name}_m%d.png'%x).resize((640,374)) for x in range($Shots)]
cols=3; rows=(len(ims)+cols-1)//cols
s=Image.new('RGB',(640*cols,374*rows))
for i,im in enumerate(ims): s.paste(im,((i%cols)*640,(i//cols)*374))
s.save(r'$runs\${Name}_sheet.png')
"@
python -c $py
"$runs\${Name}_sheet.png"
