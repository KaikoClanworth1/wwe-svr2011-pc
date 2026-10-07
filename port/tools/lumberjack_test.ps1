# Lumberjack test (src/match_types.cpp), in the limits test game
# (lim_session.ps1): PLAY -> 6-MAN -> LUMBERJACK, two picks (RIGHT, A, A; the
# 4 lumberjacks are random),
# entrances off, then screenshots every 10 s (runs\<Name>_t*.png) and the
# match types log lines.
#   lumberjack_test.ps1 [-Name lj] [-Seconds 80] [-Every 10]
param([string]$Name = "lj", [int]$Seconds = 80, [int]$Every = 10, [int]$Picks = 2, [switch]$FpsProbe)
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
if ($FpsProbe) { $env:SVR2011_FPS_PROBE = "2" }
& (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
$env:SVR2011_FPS_PROBE = $null
$nav = Join-Path $tools "lim_nav.ps1"
$step = Join-Path $tools "lim_step.ps1"
$s = Join-Path $tools "lim_session.ps1"
$log = Join-Path $runs "$Name.log"
# (each step checked in the log: the title's demo match can start if a press comes too early)
for ($t = 0; $t -lt 90 -and -not (Select-String $log -Pattern "now playing MENU THEME" -Quiet); $t++) { Start-Sleep 2 }
& $nav -Keys "BACK@3" | Out-Null
& $step -Log $log -Keys "START@3" -Expect "menu select: group 0" -Tries 8 | Out-Null
& $step -Log $log -Keys "A@3" -Expect "menu select: PLAY" | Out-Null
& $nav -Keys "DOWN@1.5,DOWN@1.5,DOWN@1.5,DOWN@1.5" | Out-Null
& $step -Log $log -Keys "A@3" -Expect "menu select: .*group 2 row 4" -Tries 1 | Out-Null   # 6-MAN
& $nav -Keys "DOWN@1.5,DOWN@1.5,DOWN@1.5,DOWN@1.5,DOWN@1.5" | Out-Null                # LUMBERJACK (row 5)
& $step -Log $log -Keys "A@5" -Expect "group A row 5" -Tries 1 | Out-Null
for ($i = 1; $i -le $Picks; $i++) { & $nav -Keys "RIGHT@1,A@2,A@3" | Out-Null }
& $nav -Keys "A@14" | Out-Null
& $nav -Keys "A@8" | Out-Null        # (the test save has Universe on: its question, YES - else PLAY)
& $s shot "${Name}_vs" | Out-Null
& $nav -Keys "DOWN@1,A@1" | Out-Null                                   # entrances off, PLAY
for ($t = $Every; $t -le $Seconds; $t += $Every) { Start-Sleep $Every; & $s shot "${Name}_t$t" | Out-Null }
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "match types|match starts" | ForEach-Object { $_.Line.Substring(50) }
& $s stop | Out-Null
