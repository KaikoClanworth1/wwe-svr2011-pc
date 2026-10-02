# Custom arenas with more Superstars (arenas branch): an exhibition 6-MAN
# BATTLE ROYAL (player 1 John Cena, five COM picks) in a custom arena, with
# the heap log on (SVR2011_TEST_HEAP_LOG: failed allocations), screenshots
# runs\<Name>_*.png and the log's problems at the end.
#   big_match_test.ps1 [-Name bm1] [-Tile 1] [-Shots 16]
# -Tile: the custom arena's tile on page 2 (0-4, top row: 1 RAW, 4 Superstars).
# In SELECT ARENA the cursor starts on the top right tile: RIGHT -> page 2
# (its first tile), RIGHT x Tile.
param([string]$Name = "bm1", [int]$Tile = 1, [int]$Shots = 16, [switch]$SkipEntrances)
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_arena"
robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
$env:SVR2011_TEST_HEAP_LOG = "1"
$s = Join-Path $tools "arena_session.ps1"
$nav = Join-Path $tools "arena_nav.ps1"
try {
    & $s start -Name $Name | Out-Null
    & $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null                                   # main menu
    & $nav -Keys "A@5,DOWN@1.5,DOWN@1.5,DOWN@1.5,DOWN@1.5,A@3,A@8" | Out-Null                # PLAY > 6-MAN > BATTLE ROYAL
    & $nav -Keys "UP@1.5,LEFT@1.5,A@4,A@3,A@3,A@3,A@3,A@3,A@3,A@3,A@3,A@3,A@3,A@4,A@6" -Shot "${Name}_vs0" | Out-Null  # 6 picks
    & $nav -Keys "RIGHT@1.5,RIGHT@1.5,A@7,RIGHT@3" | Out-Null                                 # SELECT ARENA, page 2
    if ($Tile -gt 0) { & $nav -Keys ((1..$Tile | ForEach-Object { "RIGHT@1.5" }) -join ",") | Out-Null }
    & $s shot "${Name}_p2" | Out-Null
    & $nav -Keys "A@6" -Shot "${Name}_vs1" | Out-Null                                        # chosen: the VS screen
    & $nav -Keys "LEFT@1.5,LEFT@1.5,A@1" | Out-Null                                          # PLAY
    Start-Sleep 25                                                                            # (the arena loads)
    & $s shot "${Name}_e0" | Out-Null
    if ($SkipEntrances) { & $nav -Keys ((1..14 | ForEach-Object { "A@5" }) -join ",") | Out-Null }  # A skips each entrance
    for ($i = 0; $i -lt $Shots; $i++) { Start-Sleep 10; & $s shot "${Name}_m$i" | Out-Null }
} finally {
    Remove-Item Env:SVR2011_TEST_HEAP_LOG -ErrorAction SilentlyContinue
    & $s stop | Out-Null
}
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "arena select:|BG\d\d ->|FAILED \(|access violation|fps: " |
    Where-Object { $_.Line -notmatch "VS screen scan" } | Select-Object -Last 14 | ForEach-Object { $_.Line.Substring(26, [Math]::Min(160, $_.Line.Length - 26)) }
