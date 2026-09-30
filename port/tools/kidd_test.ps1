# Tyson Kidd's roster picture after Superstar Threads (a white square, as a
# Steam Deck player saw): Create A Superstar -> Superstar Threads -> Batista ->
# Attires -> slot 1 -> Edit -> back out to the main menu, then the One on One
# roster (runs\<Name>_roster.png). -NoThreads goes straight to the roster.
# -DrawLog writes the roster frame's draws to runs\<Name>_draws.txt (native renderer).
#   kidd_test.ps1 [-Name kidd] [-Config test_config.toml] [-UserData <test saves>] [-NoThreads] [-DrawLog]
param([string]$Name = "kidd", [string]$Config = "test_config.toml", [string]$UserData, [switch]$NoThreads, [switch]$DrawLog)
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs $Config
if ($UserData) { $env:SVR2011_USER_DATA = $UserData }
$draws = Join-Path $runs "${Name}_draws.txt"
if ($DrawLog) { $env:SVR2011_DRAWLOG = $draws; Remove-Item "$draws.go" -ErrorAction SilentlyContinue }
$s = Join-Path $PSScriptRoot "session.ps1"
function K($k, $w = 3) { & $s input "press $k 200" | Out-Null; Start-Sleep $w }
& $s start -Name $Name | Out-Null
Start-Sleep 50; K START 40; K A 5                        # title -> training ring
if (-not $NoThreads) {
    K START 6; K DOWN; K DOWN; K DOWN; K A 4            # CREATE MODES
    K DOWN; K A 5; K DOWN; K DOWN; K A 8                 # CREATE A SUPERSTAR -> SUPERSTAR THREADS
    K DOWN; K A 10; K DOWN; K A 12                       # Batista -> ATTIRES
    K A 6; K A 5                                         # slot 1 -> EDIT
    & $s shot "${Name}_edit" | Out-Null
    foreach ($i in 1..6) { K B 4 }                       # back out (no save)
    & $s shot "${Name}_back" | Out-Null
    foreach ($i in 1..4) { K B 4 }
}
& $s shot "${Name}_menu" | Out-Null
K START 6; K A 4; K A 6; K A 12; K A 4                   # One on One, join
& $s shot "${Name}_roster" | Out-Null
if ($DrawLog) { Set-Content "$draws.go" "" ; Start-Sleep 4 }
