# Test: Create Modes -> Create A Superstar -> Superstar Threads -> Batista ->
# Attires -> slot 1 -> Edit -> Trunks -> a red swatch (runs\<Name>_*.png).
#   threads_test.ps1 [-Name thr] [-Config test_config.toml]
param([string]$Name = "thr", [string]$Config = "test_config.toml")
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs $Config
$s = Join-Path $PSScriptRoot "session.ps1"
function K($k, $w = 3) { & $s input "press $k 200" | Out-Null; Start-Sleep $w }
& $s start -Name $Name | Out-Null
Start-Sleep 50; K START 40; K A 5                       # title -> training ring (notice)
K START 6; K DOWN; K DOWN; K DOWN; K A 4               # main menu -> CREATE MODES
K DOWN; K A 5; K DOWN; K DOWN; K A 8                    # CREATE A SUPERSTAR -> SUPERSTAR THREADS
K DOWN; K A 10; K DOWN; K A 12                          # Batista -> ATTIRES
K A 6; K A 5; & $s shot "${Name}_edit" | Out-Null       # slot 1 -> EDIT
K DOWN 2; K DOWN 2; K DOWN 2; K A 4                     # TRUNKS
K DOWN 2; K RIGHT 2; K RIGHT 2; K A 3; & $s shot "${Name}_red" | Out-Null
