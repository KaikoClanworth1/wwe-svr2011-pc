# The whole PC night: the menu explorer (suite A), then suites B, C, D, E (night.ps1).
param([int]$Workers = 4, [int]$Monkey = 150, [string]$Suites = "B,C,D,E")
$night = Join-Path $PSScriptRoot "..\..\runs\night"
"=== explorer $(Get-Date -Format s)" | Out-File (Join-Path $night "run_night.log") -Append
python (Join-Path $PSScriptRoot "explore.py") --workers $Workers --monkey $Monkey *>> (Join-Path $night "explore.out")
"=== suites $Suites $(Get-Date -Format s)" | Out-File (Join-Path $night "run_night.log") -Append
powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "night.ps1") -Workers $Workers -Only $Suites *>> (Join-Path $night "night.out")
"=== done $(Get-Date -Format s)" | Out-File (Join-Path $night "run_night.log") -Append
