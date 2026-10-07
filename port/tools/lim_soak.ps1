# Soak runs for the limits test game (lim_session.ps1): CPU-only matches
# through the scripted route, one at a time, each until it ends, hangs
# ("world update stuck"), crashes or -Minutes pass. One line per run.
#   lim_soak.ps1 -Name b06 -Runs 4 -Match "people=A,B,C,D cpu=all arena=17" [-Rule 06] [-Mode lumberjack] [-SlowMs 14] [-Minutes 12]
param([string]$Name = "soak", [int]$Runs = 1, [string]$Match = "", [string]$Rule = "", [string]$Mode = "",
      [int]$SlowMs = 0, [int]$Minutes = 12, [string]$Config = "test_config.toml")
$tools = $PSScriptRoot
$dir = Join-Path (Split-Path $tools -Parent) "runs"
$s = Join-Path $tools "lim_session.ps1"
for ($k = 1; $k -le $Runs; $k++) {
    $env:SVR2011_CONFIG = Join-Path $dir $Config
    $env:SVR2011_USER_DATA = Join-Path $dir "test_userdata_lim"
    $env:SVR2011_ROUTE = "match"
    if ($Rule) { $env:SVR2011_TEST_RULE = $Rule }
    if ($Match) { $env:SVR2011_TEST_MATCH = $Match }
    if ($Mode) { $env:SVR2011_TEST_MODE = $Mode }
    if ($SlowMs -gt 0) { $env:SVR2011_TEST_SLOW_MS = "$SlowMs"; $env:SVR2011_TEST_SLOW_IN_MATCH = "1" }
    $run = "${Name}_$k"
    try { & $s start -Name $run | Out-Null } catch { "blocked: $_"; break }
    foreach ($v in "ROUTE", "TEST_RULE", "TEST_MATCH", "TEST_MODE", "TEST_SLOW_MS", "TEST_SLOW_IN_MATCH") { [Environment]::SetEnvironmentVariable("SVR2011_$v", $null) }
    $log = Join-Path $dir "$run.log"
    $t0 = Get-Date
    $pattern = "world update stuck|crash: SvR|match end: step 1|access violation"
    while (((Get-Date) - $t0).TotalMinutes -lt $Minutes -and -not (Select-String $log -Pattern $pattern -Quiet)) { Start-Sleep 5 }
    Start-Sleep 3
    if (Select-String $log -Pattern "world update stuck" -Quiet) { Start-Sleep 5 }  # (the thread dump)
    & $s stop | Out-Null
    $hit = Select-String $log -Pattern $pattern | Select-Object -First 1
    $text = if ($hit) { $hit.Line.Substring(40, [math]::Min(140, $hit.Line.Length - 40)) } else { "ran $Minutes min" }
    "{0}: {1:N1} min | {2}" -f $run, ((Get-Date) - $t0).TotalMinutes, $text
    Start-Sleep 3
}
