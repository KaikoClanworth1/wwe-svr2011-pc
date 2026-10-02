# Arena select research: goes to the Universe match screen (as
# arena_load_test.ps1), opens SELECT ARENA, moves the cursor, and takes
# screenshots (runs\<Name>_s*.png). Pass watch ranges via SVR2011_TEST_WATCH.
#   arena_select_probe.ps1 [-Name as1] [-Keys "RIGHT,RIGHT,DOWN"]
param([string]$Name = "as1", [string]$Keys = "RIGHT,RIGHT,RIGHT,DOWN,DOWN,LEFT")
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_arena"
robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
$s = Join-Path $tools "arena_session.ps1"
$nav = Join-Path $tools "arena_nav.ps1"
& $s start -Name $Name | Out-Null
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null
& $nav -Keys "A@3,A@3,A@8,A@3,A@4,A@4,A@5,A@6" -Shot "${Name}_menu" | Out-Null
& $nav -Keys "RIGHT@2,RIGHT@2,A@5" -Shot "${Name}_s0" | Out-Null
$i = 1
foreach ($k in ($Keys -split ",")) { & $nav -Keys "$k@2" -Shot "${Name}_s$i" | Out-Null; $i++ }
& $s stop | Out-Null
"done"
