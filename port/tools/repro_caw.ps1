# Boots (test saves), goes to Create A Superstar -> NEW -> first superstar
# template -> HEAD -> TATTOOS, then presses the given buttons, one per second
# and a half, with a screenshot after each. Reports whether the game crashed.
#   repro_caw.ps1 -Name <run> [-Renderer main|off] [-Then DOWN,A]
param([Parameter(Mandatory)] [string]$Name, [string]$Renderer = "main", [string[]]$Then = @("DOWN", "A"))
$port = Split-Path $PSScriptRoot -Parent
$s = Join-Path $PSScriptRoot "session.ps1"
function P($b, $w) { & $s input "press $b 200" | Out-Null; Start-Sleep -Milliseconds ([int]($w * 1000)) }
& $s start -Name $Name "--native_renderer=$Renderer" | Out-Null
Start-Sleep 45
P START 40; P A 5; P START 10                         # title -> main menu
P DOWN 1.5; P DOWN 1.5; P DOWN 1.5; P A 3             # CREATE MODES
P DOWN 1.5; P A 3; P A 30                             # CREATE A SUPERSTAR -> NEW
P A 4; P A 8                                          # SUPERSTAR, first template
P UP 1.5; P A 5                                       # HEAD -> TATTOOS
& $s shot "$Name`_0" | Out-Null
$i = 1
foreach ($b in $Then) { P $b 3; & $s shot "$Name`_$i" 2>$null | Out-Null; $i++ }
Start-Sleep 3
$alive = [bool](Get-CimInstance Win32_Process -Filter "Name='svr2011.exe'")
"alive: $alive"
Select-String -Path (Join-Path $port "runs\$Name.log") -Pattern "violation|FatalError" | ForEach-Object { $_.Line }
if ($alive) { & $s stop | Out-Null }
