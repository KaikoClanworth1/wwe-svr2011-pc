# Boots (test saves) into Create Modes -> Create A Superstar -> PAINT TOOL
# (the slot grid), then presses the given buttons, 3 s apart, with a
# screenshot after each.   repro_paint.ps1 -Name <run> [-Then Y,A] [-Env @{K='v'}]
param([Parameter(Mandatory)] [string]$Name, [string[]]$Then = @(), [hashtable]$Env = @{}, [switch]$Keep)
$port = Split-Path $PSScriptRoot -Parent
$s = Join-Path $PSScriptRoot "session.ps1"
function P($b, $w) { & $s input "press $b 200" | Out-Null; Start-Sleep -Milliseconds ([int]($w * 1000)) }
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }
& $s start -Name $Name "--native_renderer=main" | Out-Null
foreach ($k in $Env.Keys) { Remove-Item "Env:$k" }
Start-Sleep 45
P START 40; P A 5; P START 10                         # title -> main menu
P DOWN 1.5; P DOWN 1.5; P DOWN 1.5; P A 3             # CREATE MODES
P DOWN 1.5; P A 3                                     # CREATE A SUPERSTAR
P DOWN 1.5; P DOWN 1.5; P DOWN 1.5; P A 20            # PAINT TOOL
& $s shot "$Name`_0" | Out-Null
$i = 1
foreach ($b in $Then) { P $b 3; & $s shot "$Name`_$i" 2>$null | Out-Null; $i++ }
$alive = [bool](Get-CimInstance Win32_Process -Filter "Name='svr2011.exe'")
"alive: $alive"
Select-String -Path (Join-Path $port "runs\$Name.log") -Pattern "violation|NtWriteFile|watch" | ForEach-Object { $_.Line.Substring([Math]::Min(26, $_.Line.Length)) }
if ($alive -and -not $Keep) { & $s stop | Out-Null }
