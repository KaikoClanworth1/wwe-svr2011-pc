# Cheat-search for the arena select cursor: opens SELECT ARENA (as
# arena_select_probe.ps1), then moves the cursor and writes the expected
# cursor index to <game>\scan.txt after each move (SVR2011_TEST_SCAN).
#   arena_scan_probe.ps1 [-Name sc1]
param([string]$Name = "sc1")
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$game = Join-Path $runs "opt_arena_game"
$scan = Join-Path $game "scan.txt"
if (Test-Path $scan) { [IO.File]::Delete($scan) }
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_arena"
$env:SVR2011_TEST_SCAN = "1"
robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
$s = Join-Path $tools "arena_session.ps1"
$nav = Join-Path $tools "arena_nav.ps1"
& $s start -Name $Name | Out-Null
$env:SVR2011_TEST_SCAN = $null
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null
& $nav -Keys "A@3,A@3,A@8,A@3,A@4,A@4,A@5,A@6" | Out-Null
& $nav -Keys "RIGHT@2,RIGHT@2,A@5" | Out-Null      # SELECT ARENA, cursor on the show's arena (index 4)
# the cursor starts on the current show's arena (Superstars, index 4)
$steps = @(@("", 4), @("LEFT", 3), @("LEFT", 2), @("DOWN", 7), @("DOWN", 12), @("RIGHT", 13), @("UP", 8))
foreach ($st in $steps) {
    if ($st[0]) { & $nav -Keys "$($st[0])@2" | Out-Null }
    [IO.File]::WriteAllText($scan, "$($st[1])")
    Start-Sleep 8
}
& $s shot "${Name}_end" | Out-Null
& $s stop | Out-Null
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "\] scan " | ForEach-Object { $_.Line.Substring(26, [Math]::Min(500, $_.Line.Length - 26)) }
