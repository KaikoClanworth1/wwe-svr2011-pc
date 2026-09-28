# Debug: plays into a One on One match and, when the native window shows an
# (almost) all-white frame, triggers a draw dump of the next native frame
# (SVR2011_NATIVE_DUMP_TRIGGER) -> runs\native_draws_white.txt.
$ErrorActionPreference = "Continue"
$port = Split-Path $PSScriptRoot -Parent
$runsDir = Join-Path $port "runs"
$trig = Join-Path $runsDir "dump_trigger"
if (Test-Path $trig) { [System.IO.File]::Delete($trig) }
$env:SVR2011_NATIVE_DUMP_TRIGGER = $trig
$env:SVR2011_NATIVE_LOG_UP = "1"
$t = Join-Path $PSScriptRoot "session.ps1"
& $t start -Name play3 "--native_renderer=shadow" | Out-Null
Start-Sleep 40; & $t input "press START 200" "wait 6000" "press A 200" | Out-Null
Start-Sleep 12; & $t input "press A 200" | Out-Null
Start-Sleep 30; & $t input "press A 200" | Out-Null
Start-Sleep 30; & $t input "press START 200" | Out-Null
Start-Sleep 5
foreach ($k in 1..8) { & $t input "press A 200" | Out-Null; Start-Sleep 7 }
$found = $false
foreach ($k in 1..60) {
    Start-Sleep 1
    if ($k % 7 -eq 0) { & $t input "press A 200" | Out-Null }
    & $t shotnative "w_probe" | Out-Null
    $b = python -c "from PIL import Image; a=Image.open(r'$runsDir\w_probe.png').convert('L'); a=a.crop((0,a.size[1]-700,a.size[0],a.size[1])); print(int(sum(a.resize((32,18)).getdata())/576))" 2>$null
    if ([int]$b -gt 180) {
        "white at probe $k (brightness $b)"
        New-Item -ItemType File $trig | Out-Null
        Start-Sleep 3
        & $t shot "w_emu" | Out-Null; & $t shotnative "w_nat" | Out-Null
        $found = $true; break
    }
}
Start-Sleep 2
& $t stop | Out-Null
Start-Sleep 4
if ($found) {
    Move-Item (Join-Path (Split-Path $port -Parent) "Game Files\native_draws.txt") (Join-Path $runsDir "native_draws_white.txt") -Force
    (Get-Item (Join-Path $runsDir "native_draws_white.txt")).Length
} else { "no white frame seen" }
