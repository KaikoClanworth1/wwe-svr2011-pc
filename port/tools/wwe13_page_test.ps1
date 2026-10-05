# arena_page_test.ps1 for the WWE '13 arena work, on its own game (runs\opt_wwe13_game),
# own saves (runs\test_userdata_wwe13) and own session state. Muted, background.
#   wwe13_page_test.ps1 [-Name w13] [-Shots 12] [-Right 0]
param([string]$Name = "w13", [int]$Shots = 12, [int]$Right = 0, [int]$Turn = 3, [int]$Picks = 5, [switch]$NoWait)
$tools = "D:\Xbox Games Ports\SvR2011 Arenas\port\tools"
# never beside another game: wait until none runs (never stop one)
if (-not $NoWait) { while (Get-Process svr2011,svr2011_trace -ErrorAction SilentlyContinue) { Start-Sleep 10 } }
$runs = "D:\Xbox Games Ports\SvR2011 Arenas\port\runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_wwe13"
robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
$s = Join-Path $tools "wwe13_session.ps1"
$nav = Join-Path $tools "wwe13_nav.ps1"
# the route (src/script_input.cpp) waits for each menu, so the timing can't slip
$env:SVR2011_ROUTE = "normal"
& $s start -Name $Name | Out-Null
Remove-Item Env:SVR2011_ROUTE
$log = Join-Path $runs "$Name.log"
for ($t = 0; $t -lt 180; $t += 2) {
    Start-Sleep 2
    if (Select-String -Path $log -Pattern "script input: all steps done" -Quiet) { break }
}
Start-Sleep 5
& $s shot "${Name}_s0" | Out-Null
for ($k = 1; $k -le $Picks; $k++) { & $nav -Keys "A@6" -Shot "${Name}_s$k" | Out-Null }   # select screen -> match screen
& $nav -Keys "RIGHT@3,RIGHT@3" -Shot "${Name}_pa" | Out-Null
& $nav -Keys "A@12" -Shot "${Name}_p1" | Out-Null                  # SELECT ARENA, page 1
# from the cursor's first tile (the show's arena: WrestleMania, tile 2, with the
# route's Universe day) RIGHT past the edge turns to page 2
for ($k = 0; $k -lt $Turn; $k++) { & $nav -Keys "RIGHT@3" -Shot "${Name}_t$k" | Out-Null }
if ($Right -gt 0) { & $nav -Keys ((1..$Right | ForEach-Object { "RIGHT@1.5" }) -join ",") | Out-Null }
& $s shot "${Name}_p2" | Out-Null
& $nav -Keys "A@5" -Shot "${Name}_chosen" | Out-Null
& $nav -Keys "LEFT@2,LEFT@2" -Shot "${Name}_menu" | Out-Null
& $nav -Keys "A@1" | Out-Null
for ($i = 0; $i -lt $Shots; $i++) { Start-Sleep 8; & $s shot "${Name}_m$i" | Out-Null }
& $s stop | Out-Null
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "arena (mods|select)|access violation|\[(error|critical)\]" | Select-Object -First 25 | ForEach-Object { $_.Line.Substring([Math]::Min(26, $_.Line.Length)) }
