# Frame rate test (src/frame_rate.h) in a match, in the limits test game
# (lim_session.ps1): -Rule (hex; default 36 = IRON MAN) played from ONE ON ONE
# NORMAL (SVR2011_TEST_RULE) at -Fps, entrances on; screenshots at known wall
# times (runs\<Name>_e*.png entrances, runs\<Name>_m<seconds>.png match), so
# a clock on screen can be compared with real time.
#   fps_match.ps1 [-Fps 120] [-Rule 36] [-Name fpsm120]
param([int]$Fps = 120, [string]$Rule = "36", [string]$Name = "")
if (-not $Name) { $Name = "fpsm$Fps" }
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$cfg = Join-Path $runs "test_config_fps$Fps.toml"
$lines = Get-Content (Join-Path $runs "test_config.toml") | Where-Object { $_ -notmatch '^frame_rate' }
[IO.File]::WriteAllLines($cfg, $lines + "frame_rate = $Fps")
$env:SVR2011_CONFIG = $cfg
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
$env:SVR2011_FPS_PROBE = "1"
$env:SVR2011_TEST_RULE = $Rule
& (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
$env:SVR2011_FPS_PROBE = $null; $env:SVR2011_TEST_RULE = $null
$nav = Join-Path $tools "lim_nav.ps1"
$s = Join-Path $tools "lim_session.ps1"
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null
& $nav -Keys "A@3,A@3,A@8" | Out-Null
& $nav -Keys "A@3,A@4" | Out-Null
& $nav -Keys "A@4" | Out-Null
& $nav -Keys "A@5" | Out-Null
& $nav -Keys "A@6" | Out-Null
& $nav -Keys "A@1" | Out-Null                                 # PLAY (entrances on)
foreach ($t in 10, 20, 30) { Start-Sleep 10; & $s shot "${Name}_e$t" | Out-Null }
& $s input "press START 200" | Out-Null; Start-Sleep 4
& $s input "press START 200" | Out-Null; Start-Sleep 8
& $s input "press B 200" | Out-Null; Start-Sleep 3             # (out of the pause menu, if START opened it)
$t0 = Get-Date
foreach ($t in 0, 30, 60) {
  while (((Get-Date) - $t0).TotalSeconds -lt $t) { Start-Sleep -Milliseconds 200 }
  & $s shot "${Name}_m$t" | Out-Null
}
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "fps probe|frame rate" | Select-Object -Last 6 | ForEach-Object { $_.Line.Substring(26) }
& $s stop | Out-Null
