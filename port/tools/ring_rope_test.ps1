# Ring Kit rope test (arenas branch): an exhibition ONE ON ONE (player 1 on
# the select screen's first tile vs COM Randy Orton) in the test game, then
# player 1 runs at the ropes in each direction (stick + run) and the log
# records fighter 0's changing state words (SVR2011_TEST_STATE_LOG: "scan" or
# offsets) with its position. Screenshots runs\<Name>_r*.png.
#   ring_rope_test.ps1 [-Name rr] [-Fields scan] [-Ring "ring.ropes=0 0 0"] [-Rounds 2] [-Settle 45]
# Each round runs with another run input: RB, RT, LB, LT.
param([string]$Name = "rr", [string]$Fields = "scan", [string]$Ring = "", [int]$Rounds = 2, [int]$Settle = 45,
      [switch]$AutoRun)
# -AutoRun: the game starts player 1's runs itself (SVR2011_TEST_RUN) while the
# script holds every likely run input (RB+LB, both triggers) with the stick still.
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_arena"
robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
$env:SVR2011_TEST_STATE_LOG = $Fields
if ($AutoRun) { $env:SVR2011_TEST_RUN = "1" } else { Remove-Item Env:SVR2011_TEST_RUN -ErrorAction SilentlyContinue }
if ($Ring) { $env:SVR2011_TEST_RING = $Ring } else { Remove-Item Env:SVR2011_TEST_RING -ErrorAction SilentlyContinue }
$s = Join-Path $tools "arena_session.ps1"
$nav = Join-Path $tools "arena_nav.ps1"
try {
    & $s start -Name $Name | Out-Null
    & $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null        # title -> main menu
    & $nav -Keys "A@5,A@3,A@10" | Out-Null                         # PLAY -> ONE ON ONE -> NORMAL
    & $nav -Keys "UP@1.5,LEFT@1.5,A@4,UP@2,A@6,A@5,A@5" -Shot "${Name}_sel" | Out-Null   # 1P first tile, COM Orton
    & $nav -Keys "A@5,A@5,A@5" | Out-Null                          # PLAY -> start
    Start-Sleep $Settle                                            # entrances
    & $s shot "${Name}_start" | Out-Null
    $dirs = @(@(32767, 0), @(-32767, 0), @(0, 32767), @(0, -32767))
    if ($AutoRun) { $inputs = @("RB+LB 255 255") }
    if (-not $AutoRun) { $inputs = @("RB 0 0", "- 255 0", "LB 0 0", "- 0 255") }
    for ($r = 0; $r -lt $Rounds; $r++) {
        $run = $inputs[$r % $inputs.Count]
        $k = 0
        foreach ($d in $dirs) {
            & $s input "stick L $($d[0]) $($d[1]) 2500 $run" | Out-Null
            Start-Sleep -Milliseconds 1200
            & $s shot "${Name}_r$r$k" | Out-Null
            Start-Sleep -Milliseconds 2000
            $k++
        }
    }
} finally {
    Remove-Item Env:SVR2011_TEST_STATE_LOG -ErrorAction SilentlyContinue
    Remove-Item Env:SVR2011_TEST_RING -ErrorAction SilentlyContinue
    Remove-Item Env:SVR2011_TEST_RUN -ErrorAction SilentlyContinue
    & $s stop | Out-Null
}
"logged: " + (Select-String -Path (Join-Path $runs "$Name.log") -Pattern "ring (state|scan): ").Count
