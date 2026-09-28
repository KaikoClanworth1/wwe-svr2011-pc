# Debug: boots to the training ring, opens the main menu (START) and presses A
# -Presses times (7 s apart), then dumps the next native frame's draws
# (SVR2011_NATIVE_DUMP_TRIGGER -> runs\native_draws_<Name>.txt) and takes both
# screenshots (runs\<Name>_emu.png / _nat.png).
param([int]$Presses = 8, [string]$Name = "menu")
$ErrorActionPreference = "Continue"
$port = Split-Path $PSScriptRoot -Parent
$runsDir = Join-Path $port "runs"
$trig = Join-Path $runsDir "dump_trigger"
if (Test-Path $trig) { [System.IO.File]::Delete($trig) }
$env:SVR2011_NATIVE_DUMP_TRIGGER = $trig
$t = Join-Path $PSScriptRoot "session.ps1"
& $t start -Name $Name "--native_renderer=shadow" | Out-Null
Start-Sleep 40; & $t input "press START 200" "wait 6000" "press A 200" | Out-Null
Start-Sleep 12; & $t input "press A 200" | Out-Null
Start-Sleep 30; & $t input "press A 200" | Out-Null
Start-Sleep 30; & $t input "press START 200" | Out-Null
Start-Sleep 5
foreach ($k in 1..$Presses) { & $t input "press A 200" | Out-Null; Start-Sleep 7 }
& $t shot "$($Name)_emu" | Out-Null; & $t shotnative "$($Name)_nat" | Out-Null
New-Item -ItemType File $trig | Out-Null
Start-Sleep 3
& $t stop | Out-Null
Start-Sleep 4
$dump = Join-Path (Split-Path $port -Parent) "Game Files\native_draws.txt"
if (Test-Path $dump) {
    Move-Item $dump (Join-Path $runsDir "native_draws_$Name.txt") -Force
    "dump: " + (Get-Item (Join-Path $runsDir "native_draws_$Name.txt")).Length
} else { "no dump" }
