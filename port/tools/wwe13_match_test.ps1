# WWE '13 arena match test (own game runs\opt_wwe13_game, own saves and session state):
# the arena file takes bg01's place in that game folder (the original is kept in
# runs\wwe13_orig and put back at the end), then SVR2011_ROUTE=match with
# SVR2011_TEST_MATCH plays Cena vs Orton there. After the entrances, player 1
# runs, strikes, grapples and whips in turn; shots runs\<Name>_*.png.
# Never runs beside another game (waits), never stops one it did not start.
#   wwe13_match_test.ps1 -Pac <arena.pac> [-Name wm] [-Shots 30] [-Every 6] [-Settle 70] [-Acts "stick L 0 -32767 3000", ...]
param([Parameter(Mandatory = $true)][string]$Pac, [string]$Name = "wm", [int]$Shots = 30, [int]$Every = 6,
      [int]$Settle = 70, [string]$People = "JOHN CENA,RANDY ORTON", [string[]]$Acts = @(), [string]$Rule = "", [switch]$NoWait)
$port = "D:\Xbox Games Ports\SvR2011 Arenas\port"
$tools = Join-Path $port "tools"
$runs = Join-Path $port "runs"
$game = Join-Path $runs "opt_wwe13_game"
if (-not $NoWait) { while (Get-Process svr2011, svr2011_trace -ErrorAction SilentlyContinue) { Start-Sleep 10 } }
$bg = Join-Path $game "pac\bg\bg01.pac"
$orig = Join-Path $runs "wwe13_orig"
New-Item -ItemType Directory -Force $orig | Out-Null
if (-not (Test-Path (Join-Path $orig "bg01.pac"))) { Copy-Item $bg (Join-Path $orig "bg01.pac") }
Copy-Item -Force $Pac $bg
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_wwe13"
robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
$env:SVR2011_ROUTE = "match"
$env:SVR2011_TEST_MATCH = "people=$People arena=1"
if ($Rule) { $env:SVR2011_TEST_RULE = $Rule } else { Remove-Item Env:SVR2011_TEST_RULE -ErrorAction SilentlyContinue }
$s = Join-Path $tools "wwe13_session.ps1"
try {
    & $s start -Name $Name | Out-Null
    for ($i = 0; $i -lt [int]($Settle / 10); $i++) { Start-Sleep 10; & $s shot "${Name}_e$i" | Out-Null }
    # the match: a pattern of inputs, a shot after each
    $acts = @(
        "stick L 32767 0 1800 RB 0 0",        # run right (ropes)
        "press X", "press A", "stick L -32767 0 600 B 0 0",   # grapple, whip left
        "stick L 0 32767 1800 RB 0 0",        # run up
        "press X", "press X", "press A", "stick L 0 -32767 600 B 0 0",
        "stick L -32767 0 1800 RB 0 0",
        "press A", "stick L 32767 0 600 B 0 0",
        "stick L 0 -32767 2500 RB 0 0",       # towards the camera side (barrier)
        "press A", "stick L 0 -32767 600 B 0 0"
    )
    if ($Acts.Count) { $acts = $Acts }
    for ($i = 0; $i -lt $Shots; $i++) {
        & $s input $acts[$i % $acts.Count] | Out-Null
        Start-Sleep $Every
        & $s shot "${Name}_m$i" | Out-Null
    }
} finally {
    & $s stop | Out-Null
    Copy-Item -Force (Join-Path $orig "bg01.pac") $bg
    Remove-Item Env:SVR2011_ROUTE, Env:SVR2011_TEST_MATCH, Env:SVR2011_TEST_RULE -ErrorAction SilentlyContinue
}
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "match: rule|test match|script input|access violation|\[critical\]" | Select-Object -First 20 | ForEach-Object { $_.Line.Substring([Math]::Min(26, $_.Line.Length)) }
