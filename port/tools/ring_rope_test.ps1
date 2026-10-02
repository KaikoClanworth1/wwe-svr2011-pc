# Ring Kit rope test (arenas branch): a ONE ON ONE match in the test game
# (as arena_load_test.ps1), then player 1 runs at the ropes in each direction
# (stick + run) and the log records each fighter's state words and position
# (SVR2011_TEST_STATE_LOG). Screenshots runs\<Name>_r*.png.
#   ring_rope_test.ps1 [-Name rr] [-Fields "212,216"] [-Ring "ring.ropes=0 0 0"] [-Run RB] [-Trigger 0]
param([string]$Name = "rr", [string]$Fields = "212", [string]$Ring = "", [string]$Run = "RB", [int]$Trigger = 0,
      [int]$Rounds = 2)
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_arena"
$env:SVR2011_TEST_STATE_LOG = $Fields
if ($Ring) { $env:SVR2011_TEST_RING = $Ring } else { Remove-Item Env:SVR2011_TEST_RING -ErrorAction SilentlyContinue }
$s = Join-Path $tools "arena_session.ps1"
$nav = Join-Path $tools "arena_nav.ps1"
try {
    & $s start -Name $Name | Out-Null
    & $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null
    & $nav -Keys "A@3,A@3,A@8,A@3,A@4,A@4,A@5,A@6" | Out-Null
    & $nav -Keys "A@1" | Out-Null                                 # PLAY (entrances on)
    Start-Sleep 50                                                 # entrances
    & $s shot "${Name}_start" | Out-Null
    $dirs = @(@(32767, 0), @(-32767, 0), @(0, 32767), @(0, -32767))
    for ($r = 0; $r -lt $Rounds; $r++) {
        $k = 0
        foreach ($d in $dirs) {
            & $s input "stick L $($d[0]) $($d[1]) 2500 $Run $Trigger" | Out-Null
            Start-Sleep -Milliseconds 1200
            & $s shot "${Name}_r$r$k" | Out-Null
            Start-Sleep -Milliseconds 2000
            $k++
        }
    }
} finally {
    Remove-Item Env:SVR2011_TEST_STATE_LOG -ErrorAction SilentlyContinue
    Remove-Item Env:SVR2011_TEST_RING -ErrorAction SilentlyContinue
    & $s stop | Out-Null
}
$log = Join-Path $runs "$Name.log"
"states logged: " + (Select-String -Path $log -Pattern "ring state").Count
