# Frame-rate comparison dumps (src/frame_rate.cpp, SVR2011_FPS_DUMP): the
# training ring (after the title) at -Fps, nobody touching anything, and
# wrestlers 1 and 2 dumped every 100 ms for -Seconds into runs\<Name>.dump;
# tools/rate_diff.py compares two dumps.
#   rate_dump.ps1 [-Fps 120] [-Seconds 20] [-Name rd120]
param([int]$Fps = 120, [int]$Seconds = 20, [string]$Name = "", [int]$Fixed = 0, [int]$Idle = 0)
if (-not $Name) { $Name = "rd$Fps" }
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$cfg = Join-Path $runs "test_config_fps$Fps.toml"
$lines = Get-Content (Join-Path $runs "test_config.toml") | Where-Object { $_ -notmatch '^frame_rate' }
[IO.File]::WriteAllLines($cfg, $lines + "frame_rate = $Fps")
$dump = Join-Path $runs "$Name.dump"
Remove-Item $dump -ErrorAction SilentlyContinue
$env:SVR2011_CONFIG = $cfg
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
$env:SVR2011_FPS_DUMP = $dump
if ($Fixed) { $env:SVR2011_FIXED_TICK = "$Fixed"; $env:SVR2011_FIXED_IDLE = "$Idle" }
& (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
$nav = Join-Path $tools "lim_nav.ps1"
$s = Join-Path $tools "lim_session.ps1"
& $nav -Keys "BACK@45,START@40,A@12" | Out-Null              # title -> the training ring
Start-Sleep 3
Remove-Item $dump -ErrorAction SilentlyContinue                 # (from here on: the ring only)
& $s shot "${Name}_ring" | Out-Null
Start-Sleep ($Seconds / 2)
& $s shot "${Name}_mid" | Out-Null
Start-Sleep ($Seconds / 2)
& $s stop | Out-Null
$env:SVR2011_FPS_DUMP = $null; $env:SVR2011_FIXED_TICK = $null; $env:SVR2011_FIXED_IDLE = $null
