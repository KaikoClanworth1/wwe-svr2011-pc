# Presses buttons in the running session (1.5 s apart, or -Wait), then a screenshot.
#   nav.ps1 -Shot <stem> -Keys "DOWN A ..."
param([Parameter(Mandatory)] [string]$Shot, [string]$Keys = "", [double]$Wait = 1.5)
$Keys = $Keys -split "[ ,]+"
$s = Join-Path $PSScriptRoot "session.ps1"
foreach ($k in $Keys) { if ($k) { & $s input "press $k 200" | Out-Null; Start-Sleep -Milliseconds ([int]($Wait * 1000)) } }
Start-Sleep 1
& $s shot $Shot | Out-Null
