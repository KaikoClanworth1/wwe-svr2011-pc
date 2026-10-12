# Walk-speed check (player report: CPU opponents move in slow motion at 30
# fps): a match UNDERTAKER (P1, idle) vs a CPU at -Fps, wrestlers 1 and 2
# dumped every 100 ms (SVR2011_FPS_DUMP) into runs\<Name>.dump for
# -Seconds of the match (-Cpu: both CPUs); tools/lim_walk.py prints how fast each one moves.
#   .\lim_walk.ps1 [-Fps 30] [-Name wk30] [-Seconds 50] [-People "UNDERTAKER,BATISTA"]
param([int]$Fps = 30, [string]$Name = "", [int]$Seconds = 50, [string]$People = "UNDERTAKER,BATISTA", [string]$Extra = "", [switch]$Cpu)
if (-not $Name) { $Name = "wk$Fps" }
$tools = $PSScriptRoot; $runs = Join-Path (Split-Path $tools -Parent) "runs"
$s = Join-Path $tools "lim_session.ps1"
$cfg = Join-Path $runs "test_config_lim_fps$Fps.toml"
$lines = Get-Content (Join-Path $runs "test_config.toml") | Where-Object { $_ -notmatch '^(frame_rate|mnk_mode)' }
[IO.File]::WriteAllLines($cfg, $lines + "frame_rate = $Fps" + "mnk_mode = false" + ($Extra -split ';' | Where-Object { $_ }))
$dump = Join-Path $runs "$Name.dump"; Remove-Item $dump -ErrorAction SilentlyContinue
$env:SVR2011_CONFIG = $cfg; $env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"; $env:SVR2011_WINDOW_BEHIND = "1"
$env:SVR2011_FPS_DUMP = $dump
$env:SVR2011_TEST_MATCH = "people=$People arena=1" + $(if ($Cpu) { " cpu=all" } else { "" })
& $s start -Name $Name | Out-Null
try { (Get-Process -Id ((Get-Content (Join-Path $runs "lim_session.json") | ConvertFrom-Json).pid)).PriorityClass = "BelowNormal" } catch {}
& $s input "route match" | Out-Null
$log = Join-Path $runs "$Name.log"
for ($t = 0; $t -lt 150; $t++) { Start-Sleep 2; if (Select-String -Path $log -Pattern ("\] match: rule.*" + ($People -split ",")[0]) -Quiet -ErrorAction SilentlyContinue) { break } }
Start-Sleep $Seconds
& $s shot "${Name}_end" | Out-Null
& $s stop | Out-Null
Remove-Item Env:SVR2011_FPS_DUMP, Env:SVR2011_TEST_MATCH
"games: " + (Get-CimInstance Win32_Process -Filter "Name='svr2011.exe'" | Where-Object { $_.CommandLine -match "runs\\lim_" } | Measure-Object).Count
Select-String -Path $log -Pattern "\] match: rule|frame rate:|stuck" | Select-Object -Last 8 | ForEach-Object { $_.Line.Substring([math]::Min(48, $_.Line.Length)) }
