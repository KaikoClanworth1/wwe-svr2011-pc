# Arena select pages test (arenas test game): a custom arena installed in
# runs\opt_arena_game\Mods\Arenas\<id>\ shows on page 2; picking it plays it.
# Opens SELECT ARENA (cursor on the show's arena, top right), RIGHT -> page 2
# (cursor on its first tile), screenshot, A to choose, PLAY, match shots.
#   arena_page_test.ps1 [-Name pg1] [-Shots 12] [-Right 0]
# -Right: more RIGHT presses on page 2 (a custom arena sits on the tile of the
# arena it was made from: 4 for one made from Superstars, top right)
param([string]$Name = "pg1", [int]$Shots = 12, [int]$Right = 0)
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_arena"
robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
$s = Join-Path $tools "arena_session.ps1"
$nav = Join-Path $tools "arena_nav.ps1"
& $s start -Name $Name | Out-Null
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null
& $nav -Keys "A@3,A@3,A@8,A@3,A@4,A@4,A@5,A@6" | Out-Null
& $nav -Keys "RIGHT@2,RIGHT@2,A@5" -Shot "${Name}_p1" | Out-Null   # SELECT ARENA, page 1
& $nav -Keys "RIGHT@3" | Out-Null                                  # past the right edge: page 2
if ($Right -gt 0) { & $nav -Keys ((1..$Right | ForEach-Object { "RIGHT@1.5" }) -join ",") | Out-Null }
& $s shot "${Name}_p2" | Out-Null
& $nav -Keys "A@5" -Shot "${Name}_chosen" | Out-Null               # choose the custom arena
& $nav -Keys "LEFT@2,LEFT@2" -Shot "${Name}_menu" | Out-Null       # back to PLAY
& $nav -Keys "A@1" | Out-Null
for ($i = 0; $i -lt $Shots; $i++) { Start-Sleep 8; & $s shot "${Name}_m$i" | Out-Null }
& $s stop | Out-Null
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "arena (mods|select)|access violation" | ForEach-Object { $_.Line.Substring(26) }
