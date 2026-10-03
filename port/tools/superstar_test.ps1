# Superstar mods test (arenas branch): exhibition ONE ON ONE; player 1 opens
# the M (EXTRA) tile, steps -Down entries (the 5 managers come first, then
# the superstar mods) and picks one; COM picks; the match plays. Screenshots
# runs\<Name>_*.png, then the log's superstar / managers lines.
# -NoSkip: no A after PLAY (the entrances play).
# -Walk N: only walks the EXTRA list (N downs, a screenshot each: <Name>_wNN).
# -Com: COM's pick goes through the M tile too (the same entry).
#   superstar_test.ps1 [-Name ss1] [-Down 5] [-Shots 10] [-Com]
param([string]$Name = "ss1", [int]$Down = 5, [int]$Shots = 10, [switch]$Com, [switch]$NoSkip, [int]$Walk = 0, [string]$UserData = "", [int]$MatchDown = 0)
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
# -MatchDown N: that many DOWNs in the match category list (0 ONE ON ONE, 1 TAG TEAM, ...)
# -UserData <folder under runs>: that save folder as it is (no restore)
if ($UserData) { $env:SVR2011_USER_DATA = Join-Path $runs $UserData }
else {
    $env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_arena"
    robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
}
$s = Join-Path $tools "arena_session.ps1"
$nav = Join-Path $tools "arena_nav.ps1"
try {
    & $s start -Name $Name | Out-Null
    & $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null                 # main menu
    $cat = if ($MatchDown -gt 0) { ((1..$MatchDown | ForEach-Object { "DOWN@1.5" }) -join ",") + "," } else { "" }
    & $nav -Keys ("A@5," + $cat + "A@3,A@10,A@4") -Shot "${Name}_mode" | Out-Null   # PLAY > category > first type, 1P side
    & $nav -Keys "DOWN@1.5,DOWN@1.5,RIGHT@1.5,RIGHT@1.5,A@4" -Shot "${Name}_list" | Out-Null   # the M tile
    if ($Walk -gt 0) {
        for ($i = 1; $i -le $Walk; $i++) { & $nav -Keys "DOWN@1.2" -Shot ("{0}_w{1:D2}" -f $Name, $i) | Out-Null }
        return
    }
    if ($Down -gt 0) { & $nav -Keys ((1..$Down | ForEach-Object { "DOWN@1.5" }) -join ",") | Out-Null }
    & $s shot "${Name}_pick" | Out-Null
    & $nav -Keys "A@4" -Shot "${Name}_picked" | Out-Null                   # pick (attire)
    if ($Com) {
        & $nav -Keys "A@4" -Shot "${Name}_ready" | Out-Null                # ready
        # (COM's cursor starts on Randy Orton, row 1 column 11; M is 8 tiles left of it on row 2: the SM tile is two wide)
        & $nav -Keys ("DOWN@1.5," + ((1..8 | ForEach-Object { "LEFT@1" }) -join ",")) -Shot "${Name}_comtile" | Out-Null
        & $nav -Keys "A@4" -Shot "${Name}_comlist" | Out-Null
        if ($Down -gt 0) { & $nav -Keys ((1..$Down | ForEach-Object { "DOWN@1.5" }) -join ",") -Shot "${Name}_compick" | Out-Null }
        & $nav -Keys "A@4" -Shot "${Name}_compicked" | Out-Null
        & $nav -Keys "A@5" -Shot "${Name}_vs" | Out-Null                   # COM ready
    } else {
        & $nav -Keys "A@4,A@4,A@5,A@5" -Shot "${Name}_vs" | Out-Null      # ready, COM picks + ready
    }
    # (without -Com PLAY is already pressed: these skip the entrances)
    if (-not $NoSkip) { & $nav -Keys "A@5,A@5" | Out-Null }
    elseif ($Com) { & $nav -Keys "A@5" | Out-Null }                       # PLAY
    for ($i = 0; $i -lt $Shots; $i++) { Start-Sleep 8; & $s shot "${Name}_m$i" | Out-Null }
} finally {
    & $s stop | Out-Null
}
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "superstar mods|managers|access violation" |
    ForEach-Object { $_.Line.Substring(26, [Math]::Min(170, $_.Line.Length - 26)) }
