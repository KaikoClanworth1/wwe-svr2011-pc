# Phase 0 load test in the arenas test game (arena_session.ps1): ONE ON ONE
# NORMAL (default arena), screenshots during the entrances and the match
# (runs\<Name>_m*.png), then the log's errors. Muted, background, never
# focused; refuses to start while a player's game runs.
#   arena_load_test.ps1 [-Name al] [-Shots 8]
param([string]$Name = "al", [int]$Shots = 8, [string]$LogLevel = "info", [string[]]$GameArgs = @())
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_arena"
# the same Universe day every run (a played match moves Universe on)
robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
$s = Join-Path $tools "arena_session.ps1"
$nav = Join-Path $tools "arena_nav.ps1"
& $s start -Name $Name -LogLevel $LogLevel @GameArgs | Out-Null
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null
& $nav -Keys "A@3,A@3,A@8" -Shot "${Name}_menu1" | Out-Null
& $nav -Keys "A@3,A@4" -Shot "${Name}_menu2" | Out-Null
& $nav -Keys "A@4" -Shot "${Name}_menu3" | Out-Null
& $nav -Keys "A@5" -Shot "${Name}_menu4" | Out-Null
& $nav -Keys "A@6" -Shot "${Name}_menu5" | Out-Null
& $nav -Keys "A@1" | Out-Null                                 # PLAY (entrances on)
for ($i = 0; $i -lt $Shots; $i++) { Start-Sleep 8; & $s shot "${Name}_m$i" | Out-Null }
$log = Join-Path $runs "$Name.log"
Select-String -Path $log -Pattern "\[(error|critical)\]" | Where-Object { $_.Line -notmatch "BaseHeap|PhysicalHeap" } | Select-Object -First 15 | ForEach-Object { $_.Line }
& $s stop | Out-Null
"done: $runs\${Name}_m*.png"
