# Searches guest memory for byte patterns while SELECT ARENA is open
# (SVR2011_TEST_SCAN, "hex:" requests via <game>\scan.txt).
#   arena_find_probe.ps1 -Patterns "4d190b195ce000ab","194d190be05cab00" [-Name fp1]
param([string[]]$Patterns, [string]$Name = "fp1")
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$scan = Join-Path $runs "arena_game\scan.txt"
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
& $nav -Keys "RIGHT@2,RIGHT@2,A@5" -Shot "${Name}_sel" | Out-Null
foreach ($p in $Patterns) { [IO.File]::WriteAllText($scan, "hex:$p"); Start-Sleep 10 }
& $s stop | Out-Null
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "\] find " | ForEach-Object { $_.Line.Substring(26, [Math]::Min(600, $_.Line.Length - 26)) }
