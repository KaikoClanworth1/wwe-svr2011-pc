# Superstar Threads: Batista -> Attires -> slot 1 -> Edit -> Trunks -> white -> Continue -> Save "TEST THREAD";
# then a One on One with Batista in that attire (screenshot <Name>_attire.png).
#   threads_save_test.ps1 -Name x -Config test_config.toml -UserData <folder copy of test saves> [-GameDir <install>]
param([string]$Name = "thrs", [string]$Config = "test_config.toml", [string]$UserData, [string]$GameDir = "")
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs $Config
if ($UserData) { $env:SVR2011_USER_DATA = $UserData }
$s = Join-Path $PSScriptRoot "session.ps1"
function K($k, $w = 3) { & $s input "press $k 200" | Out-Null; Start-Sleep $w }
& $s start -Name $Name -GameDir $GameDir | Out-Null
Start-Sleep 50; K START 40; K A 5                        # title -> training ring
K START 6; K DOWN; K DOWN; K DOWN; K A 4                # CREATE MODES
K DOWN; K A 5; K DOWN; K DOWN; K A 8                     # CREATE A SUPERSTAR -> SUPERSTAR THREADS
K DOWN; K A 10; K DOWN; K A 12                           # Batista -> ATTIRES
K A 6; K A 5                                             # slot 1 -> EDIT
K DOWN 2; K DOWN 2; K DOWN 2; K A 4                      # TRUNKS
K DOWN 2; K RIGHT 2; K RIGHT 2; K A 3; K A 3; K A 3      # white swatch (picked, then kept)
K B 3; K DOWN 2; K DOWN 2; K DOWN 2; K DOWN 2; K A 4     # CONTINUE
& $s shot "${Name}_before" | Out-Null
foreach ($i in 1..6) { K DOWN 1.2 }; K A 6; K A 6        # SAVE -> name
& $s input "key BACK" "wait 400" "key BACK" "wait 400" "type TEST THREAD" "wait 800" "key ENTER" | Out-Null
Start-Sleep 10; & $s shot "${Name}_saved" | Out-Null; K A 6
foreach ($i in 1..5) { K B 3 }                           # back to the ring
K START 6; K A 4; K A 6; K A 12; K A 3                   # One on One, join
K RIGHT 2; K RIGHT 2; K A 4; K X 4; K RIGHT 5; K RIGHT 5 # Batista, ADVANCED, attire 3
& $s shot "${Name}_attire" | Out-Null
