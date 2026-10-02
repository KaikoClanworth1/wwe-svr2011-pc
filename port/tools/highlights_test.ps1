# Match-end highlights test (src/replays.cpp), in the limits test game
# (lim_session.ps1): ONE ON ONE NORMAL with player 1 left idle, so the CPU
# wins (finisher and pin: highlight clips); waits up to -Minutes for the
# highlights to start, then screenshots every 10 s (runs\<Name>_p*.png) and
# the highlight log lines.
# -Stall: the replay recorder never hands out a clip (SVR2011_TEST_HIGHLIGHT_STALL),
# as on the Steam Deck where a won match never ended.
#   highlights_test.ps1 [-Stall] [-Name hl]
param([switch]$Stall, [string]$Name = "", [int]$Minutes = 12)
if (-not $Name) { $Name = if ($Stall) { "hlstall" } else { "hl" } }
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
if ($Stall) { $env:SVR2011_TEST_HIGHLIGHT_STALL = "1" }
& (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
$env:SVR2011_TEST_HIGHLIGHT_STALL = $null
$nav = Join-Path $tools "lim_nav.ps1"
$s = Join-Path $tools "lim_session.ps1"
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null
& $nav -Keys "A@3,A@3,A@8" | Out-Null
& $nav -Keys "A@3,A@4" | Out-Null
& $nav -Keys "A@4" | Out-Null
& $nav -Keys "A@5" | Out-Null
& $nav -Keys "A@6" | Out-Null
& $nav -Keys "A@1" | Out-Null                                 # PLAY (entrances on)
Start-Sleep 30
& $s input "press START 200" | Out-Null; Start-Sleep 4
& $s input "press START 200" | Out-Null; Start-Sleep 8
& $s input "press B 200" | Out-Null; Start-Sleep 3             # (out of the pause menu, if START opened it)
$log = Join-Path $runs "$Name.log"
$t0 = Get-Date
while (((Get-Date) - $t0).TotalMinutes -lt $Minutes -and -not (Select-String -Path $log -Pattern "match-end highlights: \d+ clips" -Quiet)) { Start-Sleep 5 }
for ($t = 0; $t -le 90; $t += 10) { & $s shot "${Name}_p$t" | Out-Null; Start-Sleep 10 }
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "highlights|match time set|menu select" | ForEach-Object { $_.Line.Substring(26) }
& $s stop | Out-Null
