# B11/B12 check: a six-person match of CPUs (SVR2011_TEST_RULE, default 08
# 6-MAN TAG; -Mode weapons = WEAPONS EVERYWHERE) with the stuck-wrestler aid
# (SVR2011_TEST_STUCK_CHARS), screenshots every 30 s, then the pause menu's
# MATCH INFORMATION.
#   .\lim_b11.ps1 [-Name b11a] [-Rule 08] [-Mode weapons] [-Seconds 240] [-Fps 60]
param([string]$Name = "b11a", [string]$Rule = "08", [string]$Mode = "weapons", [int]$Seconds = 240, [int]$Fps = 60,
      [string]$People = "GOLDBERG,JOHN CENA,BATISTA,RANDY ORTON,EDGE,CHRIS JERICHO", [string]$Slot = "")
$tools = $PSScriptRoot; $runs = Join-Path (Split-Path $tools -Parent) "runs"
$s = Join-Path $tools "lim_session.ps1"
$cfg = Join-Path $runs "test_config_lim_fps$Fps.toml"
$lines = Get-Content (Join-Path $runs "test_config.toml") | Where-Object { $_ -notmatch '^frame_rate' }
[IO.File]::WriteAllLines($cfg, $lines + "frame_rate = $Fps")
$env:SVR2011_CONFIG = $cfg; $env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"; $env:SVR2011_WINDOW_BEHIND = "1"
$env:SVR2011_TEST_RULE = $Rule; $env:SVR2011_TEST_STUCK_CHARS = "1"
if ($Mode) { $env:SVR2011_TEST_MODE = $Mode } else { Remove-Item Env:SVR2011_TEST_MODE -ErrorAction SilentlyContinue }
$env:SVR2011_TEST_MATCH = "people=$People arena=1 cpu=all"
& $s start -Slot $Slot -Name $Name | Out-Null
# (normal priority: with up to 6 test games on the PC a below-normal one starves - 0.3 fps)
& $s input -Slot $Slot "route match" | Out-Null
$log = Join-Path $runs "$Name.log"
for ($t = 0; $t -lt 150; $t++) { Start-Sleep 2; if (Select-String -Path $log -Pattern ("\] match: rule.*" + ($People -split ",")[0]) -Quiet -ErrorAction SilentlyContinue) { break } }
for ($i = 1; $i -le [int]($Seconds / 30); $i++) { Start-Sleep 30; & $s shot -Slot $Slot "${Name}_$i" | Out-Null }
# the pause menu -> MATCH INFORMATION
& $s input -Slot $Slot "press START" "wait 2500" | Out-Null; Start-Sleep 3; & $s shot -Slot $Slot "${Name}_pause" | Out-Null
& $s input -Slot $Slot "press UP" "wait 800" "press UP" "wait 800" "press A" "wait 2500" | Out-Null  # (up from RETURN TO GAME: QUIT MATCH, MATCH INFORMATION)
Start-Sleep 3; & $s shot -Slot $Slot "${Name}_info" | Out-Null
& $s input -Slot $Slot "press UP" "wait 800" "press A" "wait 2500" | Out-Null; Start-Sleep 3; & $s shot -Slot $Slot "${Name}_info2" | Out-Null  # (MATCH RULES)
& $s input -Slot $Slot "press DOWN 1500" "wait 500" "press DOWN 1500" "wait 800" | Out-Null; Start-Sleep 3; & $s shot -Slot $Slot "${Name}_info3" | Out-Null  # (its later rows)
& $s stop -Slot $Slot | Out-Null; Start-Sleep 8
foreach ($v in "SVR2011_TEST_RULE", "SVR2011_TEST_MODE", "SVR2011_TEST_MATCH", "SVR2011_TEST_STUCK_CHARS") { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
"games (mine): " + (Get-CimInstance Win32_Process -Filter "Name='svr2011.exe'" | Where-Object { $_.CommandLine -match "runs\\lim_" } | Measure-Object).Count
Select-String -Path $log -Pattern "\] match: rule|WEAPONS EVERYWHERE|stuck|crash|access violation|set up like" | Select-Object -Last 20 | ForEach-Object { $_.Line.Substring([math]::Min(48, $_.Line.Length)) }
