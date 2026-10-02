# Drives the arena_session.ps1 game: presses keys one at a time (a pause after
# each), then optionally takes a screenshot runs\<Shot>.png.
#   lim_nav.ps1 -Keys "DOWN,DOWN,A" [-Wait 2] [-Shot name]
param([string]$Keys = "", [double]$Wait = 2, [string]$Shot = "")
$s = Join-Path $PSScriptRoot "arena_session.ps1"
foreach ($k in ($Keys -split "," | Where-Object { $_ })) {
    $w = $Wait
    if ($k -match "^(.+)@([\d.]+)$") { $k = $Matches[1]; $w = [double]$Matches[2] }   # KEY@seconds
    & $s input "press $k 200" | Out-Null
    Start-Sleep -Milliseconds ([int]($w * 1000))
}
if ($Shot) { & $s shot $Shot }
