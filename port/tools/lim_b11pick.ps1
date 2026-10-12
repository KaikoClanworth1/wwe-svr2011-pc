# B11 pick-up check: 6-MAN WEAPONS EVERYWHERE (rule 08), player 1 put next
# to a weapon on the floor (SVR2011_TEST_PUT_P1, default by the sledgehammer
# at 0, 54), then RB (pick up) and screenshots; the stuck-wrestler aid logs.
#   .\lim_b11pick.ps1 [-Name pk1] [-At "0,49"] [-Keys "RB"]
param([string]$Name = "pk1", [string]$At = "0,49", [string]$Keys = "RB", [string]$Rule = "08", [string]$Mode = "weapons",
      [string]$People = "GOLDBERG,JOHN CENA,BATISTA,RANDY ORTON,EDGE,CHRIS JERICHO")
$tools = $PSScriptRoot; $runs = Join-Path (Split-Path $tools -Parent) "runs"
$s = Join-Path $tools "lim_session.ps1"
$env:SVR2011_CONFIG = Join-Path $runs "test_config_lim_fps60.toml"; $env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"; $env:SVR2011_WINDOW_BEHIND = "1"
$env:SVR2011_TEST_RULE = $Rule; $env:SVR2011_TEST_STUCK_CHARS = "1"; $env:SVR2011_TEST_PUT_P1 = $At
if ($Mode) { $env:SVR2011_TEST_MODE = $Mode } else { Remove-Item Env:SVR2011_TEST_MODE -ErrorAction SilentlyContinue }
$env:SVR2011_TEST_MATCH = "people=$People arena=1"
& $s start -Name $Name | Out-Null
& $s input "route match" | Out-Null
$log = Join-Path $runs "$Name.log"
for ($t = 0; $t -lt 150; $t++) { Start-Sleep 2; if (Select-String -Path $log -Pattern ("\] match: rule.*" + ($People -split ",")[0]) -Quiet -ErrorAction SilentlyContinue) { break } }
for ($t = 0; $t -lt 60; $t++) { Start-Sleep 1; if (Select-String -Path $log -Pattern "person 0 put at" -Quiet) { break } }
Start-Sleep 1; & $s shot "${Name}_0" | Out-Null
foreach ($k in ($Keys -split ",")) { & $s input "press $k 200" | Out-Null; Start-Sleep 2; & $s shot "${Name}_$k" | Out-Null }
for ($i = 1; $i -le 8; $i++) { Start-Sleep 5; & $s input "press A 200" | Out-Null; Start-Sleep 1; & $s shot "${Name}_$i" | Out-Null }  # (A: pick up)
& $s stop | Out-Null; Start-Sleep 8
foreach ($v in "SVR2011_TEST_RULE", "SVR2011_TEST_MODE", "SVR2011_TEST_MATCH", "SVR2011_TEST_STUCK_CHARS", "SVR2011_TEST_PUT_P1") { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
"games (mine): " + (Get-CimInstance Win32_Process -Filter "Name='svr2011.exe'" | Where-Object { $_.CommandLine -match "runs\\lim_" } | Measure-Object).Count
Select-String -Path $log -Pattern "\] match: rule 0|put at|stuck since|access violation" | Select-Object -Last 12 | ForEach-Object { $_.Line.Substring([math]::Min(48, $_.Line.Length)) }
