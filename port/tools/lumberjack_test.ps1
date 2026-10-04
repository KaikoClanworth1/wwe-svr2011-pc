# Lumberjack test (src/match_types.cpp), in the limits test game
# (lim_session.ps1): PLAY -> 6-MAN -> LUMBERJACK, two picks (RIGHT, A, A; the
# 4 lumberjacks are random),
# entrances off, then screenshots every 10 s (runs\<Name>_t*.png) and the
# match types log lines.
#   lumberjack_test.ps1 [-Name lj] [-Seconds 80] [-Every 10]
param([string]$Name = "lj", [int]$Seconds = 80, [int]$Every = 10, [int]$Picks = 2)
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
$env:SVR2011_FPS_PROBE = "2"
& (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
$env:SVR2011_FPS_PROBE = $null
$nav = Join-Path $tools "lim_nav.ps1"
$s = Join-Path $tools "lim_session.ps1"
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null
& $nav -Keys "A@3,DOWN@1,DOWN@1,DOWN@1,DOWN@1,A@3" | Out-Null          # PLAY -> 6-MAN
for ($i = 0; $i -lt 5; $i++) { & $nav -Keys "DOWN@2" | Out-Null }       # LUMBERJACK (one at a time)
& $nav -Keys "A@5" | Out-Null
for ($i = 1; $i -le $Picks; $i++) { & $nav -Keys "RIGHT@1,A@2,A@3" | Out-Null }
& $nav -Keys "A@14" | Out-Null
& $s shot "${Name}_vs" | Out-Null
& $nav -Keys "DOWN@1,A@1" | Out-Null                                    # entrances off, PLAY
for ($t = $Every; $t -le $Seconds; $t += $Every) { Start-Sleep $Every; & $s shot "${Name}_t$t" | Out-Null }
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "match types|match starts" | ForEach-Object { $_.Line.Substring(50) }
& $s stop | Out-Null
