# MATCH CREATOR sweep (match_types.cpp, "everything allowed"): each line of
# -List is one match - "name;rule (hex);MATCH CREATOR record bytes;people" -
# played with the CPU only (route match, SVR2011_TEST_RULE, SVR2011_TEST_MC,
# SVR2011_TEST_MATCH cpu=all) for -Seconds after it starts, one game at a
# time. Results: runs\mc_sweep.csv (name, result, details) and a screenshot
# per match (runs\mcs_<name>.png).
#   OK       the match ran: its motions went on to the end
#   NOSTART  the match never ran (no match frame 60 in 240 s)
#   CRASH    a crash report
#   STUCK    "world update stuck"
#   FROZEN   no wrestler motion in the last 20 s (the match still or over)
#   ENDED    the match ended (a result) - fine, noted
#   mc_sweep.ps1 -List runs\mc_list.txt [-Seconds 45] [-From <name>]
param([Parameter(Mandatory)][string]$List, [int]$Seconds = 45, [string]$From = "")
$tools = $PSScriptRoot
$port = Split-Path $tools -Parent
$runs = Join-Path $port "runs"
$s = Join-Path $tools "lim_session.ps1"
$out = Join-Path $runs "mc_sweep.csv"
if (-not (Test-Path $out)) { "name,rule,mc,result,details" | Set-Content $out }
$started = -not $From
foreach ($line in Get-Content $List) {
    if (-not $line.Trim() -or $line.StartsWith("#")) { continue }
    $f = $line.Split(";")
    $name, $rule, $mc, $people = $f[0].Trim(), $f[1].Trim(), $f[2].Trim(), $f[3].Trim()
    if (-not $started) { if ($name -eq $From) { $started = $true } else { continue } }
    $env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
    $env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
    $env:SVR2011_ROUTE = "match"
    $env:SVR2011_TEST_RULE = $rule
    $env:SVR2011_TEST_MATCH = "people=$people cpu=all"
    $env:SVR2011_TEST_MC = $mc
    $env:SVR2011_TEST_MOTION_LOG = "1"
    $log = Join-Path $runs "mcs_$name.log"
    try { & $s start -Name "mcs_$name" | Out-Null } catch { "blocked: $_"; break }
    foreach ($v in "ROUTE", "TEST_RULE", "TEST_MATCH", "TEST_MC", "TEST_MOTION_LOG") { [Environment]::SetEnvironmentVariable("SVR2011_$v", $null) }
    $result = ""; $details = ""
    $t0 = Get-Date
    # (the match runs: a wrestler's motion at match frame 60 or later - entrances stay on)
    function Running { [bool](Select-String $log -Pattern "motion: person [0-9] .*\(frame ([6-9][0-9]|[0-9]{3,})," -Quiet) }
    while (((Get-Date) - $t0).TotalSeconds -lt 240) {
        if (Running) { break }
        if (Select-String $log -Pattern "crash: SvR" -Quiet) { break }
        Start-Sleep 3
    }
    if (-not (Running)) {
        $result = if (Select-String $log -Pattern "crash: SvR" -Quiet) { "CRASH" } else { "NOSTART" }
    } else {
        $t1 = Get-Date
        while (((Get-Date) - $t1).TotalSeconds -lt $Seconds -and -not (Select-String $log -Pattern "crash: SvR|world update stuck" -Quiet)) { Start-Sleep 3 }
        try { & $s shot "mcs_$name" | Out-Null } catch {}
        $lines = Get-Content $log
        if ($lines | Select-String "crash: SvR" -Quiet) { $result = "CRASH" }
        elseif ($lines | Select-String "world update stuck" -Quiet) { $result = "STUCK" }
        else {
            $last = $lines | Select-String "motion: person" | Select-Object -Last 1
            $end = $lines | Select-String "match end: step 1" -Quiet
            $age = if ($last) { ((Get-Date) - [datetime]::ParseExact($last.Line.Substring(1, 23), "yyyy-MM-dd HH:mm:ss.fff", $null)).TotalSeconds } else { 999 }
            $result = if ($end) { "ENDED" } elseif ($age -gt 20) { "FROZEN" } else { "OK" }
            $details = "last motion $([int]$age) s ago"
        }
    }
    $m = ($lines | Select-String "\] match: rule" | Select-Object -Last 1)
    if ($m) { $details += "; " + ($m.Line -replace '^.*match: ', '').Substring(0, [math]::Min(60, ($m.Line -replace '^.*match: ', '').Length)) }
    $c = ($lines | Select-String "crash:    0 " | Select-Object -First 1)
    if ($c) { $details += "; " + $c.Line.Substring($c.Line.IndexOf("crash:")) }
    "$name,$rule,""$mc"",$result,""$details""" | Add-Content $out
    "$name $result $details"
    & $s stop | Out-Null
    Start-Sleep 3
}
