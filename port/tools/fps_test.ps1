# Frame rate test (src/frame_rate.h), in the limits test game (lim_session.ps1):
# the training ring (after the title) at -Fps: wrestler 1 walks (stick held
# -Walk seconds) while SVR2011_FPS_PROBE logs game frames/s and his position
# every second (runs\<Name>.log, "fps probe"); screenshots runs\<Name>_*.png.
#   fps_test.ps1 [-Fps 120] [-Name fps120] [-Walk 3]
#   -Set "full_speed = false" (extra config lines)
param([int]$Fps = 120, [string]$Name = "", [int]$Walk = 3, [int]$Settle = 0, [string]$Run = "", [string[]]$Set = @())
if (-not $Name) { $Name = "fps$Fps" }
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$cfg = Join-Path $runs "test_config_fps$Fps.toml"
$lines = Get-Content (Join-Path $runs "test_config.toml") | Where-Object { $_ -notmatch '^frame_rate' }
[IO.File]::WriteAllLines($cfg, @($lines | Where-Object { $l = $_; -not ($Set | Where-Object { $l -match ("^" + ($_ -split ' ')[0] + " ") }) }) + "frame_rate = $Fps" + $Set)
$env:SVR2011_CONFIG = $cfg
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
$env:SVR2011_FPS_PROBE = "1"
$env:SVR2011_TEST_IN_MATCH = "1"
& (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
$env:SVR2011_FPS_PROBE = $null; $env:SVR2011_TEST_IN_MATCH = $null
$nav = Join-Path $tools "lim_nav.ps1"
$s = Join-Path $tools "lim_session.ps1"
& $nav -Keys "BACK@45,START@40,A@12" | Out-Null              # title -> the training ring
Start-Sleep $Settle
& $s shot "${Name}_ring" | Out-Null
if ($Run) {  # -Run LB: run (stick + that button), screenshots while running
  & $s input "stick L 0 -32000 $($Walk * 1000) $Run" | Out-Null
  Start-Sleep -Milliseconds 400
  for ($i = 0; $i -lt 16; $i++) { & $s shot "${Name}_r$i" | Out-Null }
  Start-Sleep ($Walk + 1)
} else {
  & $s input "stick L 32000 0 $($Walk * 1000)" | Out-Null; Start-Sleep ($Walk + 3)
}
& $s shot "${Name}_walk" | Out-Null
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "fps probe|frame rate:|SetFrameRate" | Select-Object -Last 14 | ForEach-Object { $_.Line.Substring(26) }
& $s stop | Out-Null
