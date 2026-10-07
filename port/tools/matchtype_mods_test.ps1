# Match type mods (match_types.h MatchTypeOn): screenshots of the PLAY lists
# ONE ON ONE, TRIPLE THREAT, FATAL-4-WAY and HANDICAP with the given match
# types switched off (a "disabled" file in the test game's
# Mods\MatchTypes\<id>), then a match through ONE ON ONE -> NORMAL MATCH.
#   matchtype_mods_test.ps1 -Name mtm_on
#   matchtype_mods_test.ps1 -Name mtm_off -Off slobber_knocker,elimination,mystery_opponent
param([string]$Name = "mtm", [string[]]$Off = @(), [switch]$Match,
      [string[]]$Lists = @("TWO ON TWO", "TRIPLE THREAT", "FATAL-4-WAY", "6-MAN", "HANDICAP"))
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$s = Join-Path $tools "lim_session.ps1"
$mods = Join-Path $runs "lim_game\Mods\MatchTypes"
# (the switches: only this run's ids off)
if (Test-Path $mods) {
    Get-ChildItem $mods -Directory | ForEach-Object {
        $d = Join-Path $_.FullName "disabled"
        if (Test-Path $d) { [IO.File]::Delete($d) }
    }
}
foreach ($id in $Off) {
    New-Item -ItemType Directory -Force (Join-Path $mods $id) | Out-Null
    Set-Content (Join-Path $mods "$id\disabled") "" -NoNewline
}
$log = Join-Path $runs "$Name.log"
function WaitSteps([int]$n) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 120) {
        if (((Select-String $log -Pattern "script input: all steps done" -ErrorAction SilentlyContinue) | Measure-Object).Count -ge $n) { return }
        Start-Sleep 1
    }
    "timeout waiting for steps ($n)"
}
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
& $s start -Name $Name | Out-Null
try { (Get-Process -Id ((Get-Content (Join-Path $runs "lim_session.json") | ConvertFrom-Json).pid)).PriorityClass = "BelowNormal" } catch {}
& $s input "route exhibition" | Out-Null
WaitSteps 1; Start-Sleep 2; & $s shot "${Name}_1v1" | Out-Null
$k = 1
foreach ($list in $Lists) {
    & $s input "press B" "wait 1500" "menu $list" | Out-Null
    $k++; WaitSteps $k; Start-Sleep 2
    & $s shot ("${Name}_" + ($list -replace '[^A-Z0-9]', '')) | Out-Null
}
if ($Match) {
    & $s input "press B" "wait 1500" "menu ONE ON ONE" "menu NORMAL MATCH" | Out-Null
    $k++; WaitSteps $k; Start-Sleep 3
    & $s shot "${Name}_normal" | Out-Null
}
& $s stop | Out-Null
foreach ($id in $Off) { [IO.File]::Delete((Join-Path $mods "$id\disabled")) }
Select-String $log -Pattern "match types:|script input: (all steps|no )" | ForEach-Object { $_.Line.Substring([math]::Min(45, $_.Line.Length)) }
