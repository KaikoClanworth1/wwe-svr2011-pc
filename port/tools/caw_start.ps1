# Boots (test saves) into Create A Superstar -> NEW -> first template and leaves
# the session running on the CAW edit menu (screenshot runs\<Name>_0.png).
#   caw_start.ps1 -Name <run>    then drive it with session.ps1 input / shot
param([Parameter(Mandatory)] [string]$Name)
$s = Join-Path $PSScriptRoot "session.ps1"
function P($b, $w) { & $s input "press $b 200" | Out-Null; Start-Sleep -Milliseconds ([int]($w * 1000)) }
& $s start -Name $Name "--native_renderer=main" | Out-Null
Start-Sleep 45
P START 40; P A 5; P START 10                         # title -> main menu
P DOWN 1.5; P DOWN 1.5; P DOWN 1.5; P A 3             # CREATE MODES
P DOWN 1.5; P A 3; P A 30                             # CREATE A SUPERSTAR -> NEW
P A 4; P A 8                                          # SUPERSTAR, first template
& $s shot "$Name`_0" | Out-Null
