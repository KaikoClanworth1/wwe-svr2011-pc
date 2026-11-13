# The lockstep lag budget rounds: for each link, one hidden two-game LAN round (svrtest lan: the host EDGE, the
# joiner KANE, both pressing every 3 s for -Play seconds, state dumps compared at the clocks) with both games'
# net input logs on, both games' outgoing packets delayed by the link (SVR2011_TEST_NET_DELAY "ms,jitter,loss%":
# one way = ms), then lag_budget.py over the round's folder. Results append to <Out>\budget.txt.
# The test server takes a port from 8530 (SVRTEST_SERVER_PORT_FROM, this chat's range).
# -Tunnel: both games through the tunnel (SVR2011_TEST_NET_TUNNEL=1: the internet path, each packet repeating the last
# 3 inputs so a lost one costs nothing); without it the LAN path (the game's socket straight to the peer).
# -Lead N: both games' input lead forced to N frames (SVR2011_TEST_NET_LEAD). -GameArgs: game options for both games
# (e.g. --p2p_hold_poll_ms=0 --p2p_lead_floor=3 --p2p_lead_margin_ms=12 --p2p_lead_extra=1 = the lead and holds as before the lever). -WaitQuiet: before each round, wait until no
# other chat's test game runs and the CPU is under 75% twice half a minute apart (builds at idle priority are not waited for); a round that ran more than -SlowLimit seconds under 57 takes a second (a build started) is run again, twice at most.
#   .\lag_rounds.ps1 [-Links "", "80,20,2", "150,40,5"] [-Play 180s] [-Out "E:\SvR2011 Work\lag"] [-Tag a] [-Tunnel] [-Lead 2] [-WaitQuiet]
param([string[]]$Links = @("", "80,20,2", "150,40,5"), [string]$Play = "180s", [string]$Out = "E:\SvR2011 Work\lag",
      [string]$Tag = "", [switch]$Tunnel, [int]$Lead = 0, [switch]$WaitQuiet, [int]$SlowLimit = 25, [string[]]$GameArgs = @())
$ErrorActionPreference = "Continue"
$d = $PSScriptRoot
$runs = Join-Path (Split-Path (Split-Path $d -Parent) -Parent) "runs\svrtest"
$logs = Join-Path $Out "logs"
New-Item -ItemType Directory -Force $logs | Out-Null
$env:SVRTEST_SERVER_PORT_FROM = "8530"
if (-not $env:SVR2011_AFFINITY) { $env:SVR2011_AFFINITY = "FF00" }  # (test games on cores 8-15, the house rule of 12 Nov; test_guard pins them)
function Wait-Quiet {
    # (builds run at idle priority on cores 12-15 and are not waited for; another chat's test game shares the test
    # cores 8-15 and is; after 60 min of waiting the round runs anyway - the slow-seconds re-run covers it)
    if (-not $WaitQuiet) { return }
    $calm = 0; $t0 = Get-Date
    while ($calm -lt 2 -and ((Get-Date) - $t0).TotalMinutes -lt 60) {
        $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^svr2011' })
        $load = (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average
        if ($busy.Count -eq 0 -and $load -lt 75) { $calm++ } else { $calm = 0; "waiting for a quiet PC ($(Get-Date -Format HH:mm:ss)): $($busy.Count) test game(s) of other chats, load $load%" }
        Start-Sleep -Seconds 30
    }
}
foreach ($link in $Links) {
  $name0 = "lag_" + $(if ($link) { ($link -replace ",", "-") } else { "lan" }) + $(if ($Lead) { "_lead$Lead" } else { "" }) + $(if ($Tag) { "_$Tag" } else { "" })
  for ($try = 0; $try -lt 3; $try++) {
    $name = $name0 + $(if ($try) { "_r$try" } else { "" })
    Wait-Quiet
    # (the game asks GitHub for the newest release and goes offline when it is older - this build may be; the answer
    # it caches in its user data (online_version.txt, 6 h) says "0" here: nothing newer, and no GitHub call in a test)
    foreach ($ud in "lan_ud_host", "lan_ud_join") { [IO.File]::WriteAllText((Join-Path $runs "$ud\online_version.txt"), "$([DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) 0`n") }
    $hostIn = Join-Path $logs "$name`_host_input.txt"; $joinIn = Join-Path $logs "$name`_join_input.txt"
    $envs = @("--host-env", "SVR2011_NET_INPUT_LOG=$hostIn", "--join-env", "SVR2011_NET_INPUT_LOG=$joinIn")
    if ($link) { $envs += @("--host-env", "SVR2011_TEST_NET_DELAY=$link", "--join-env", "SVR2011_TEST_NET_DELAY=$link") }
    if ($Tunnel) { $envs += @("--host-env", "SVR2011_TEST_NET_TUNNEL=1", "--join-env", "SVR2011_TEST_NET_TUNNEL=1") }
    if ($Lead) { $envs += @("--host-env", "SVR2011_TEST_NET_LEAD=$Lead", "--join-env", "SVR2011_TEST_NET_LEAD=$Lead") }
    foreach ($a in $GameArgs) { $envs += @("--arg", $a) }
    $t0 = Get-Date
    "===== $name ($(Get-Date -Format 'yyyy-MM-dd HH:mm')) link '$link' play $Play$(if ($Tunnel) { " tunnel" })$(if ($Lead) { " lead $Lead" })$(if ($GameArgs) { " " + ($GameArgs -join " ") })"
    $res = @()
    try { $res = & (Join-Path $d "svrtest_lan.ps1") --p1 EDGE --p2 KANE --play $Play --name $name --clock "600,1800,3600,5400,7200,9000" @envs 2>&1 | ForEach-Object { $_.ToString() } }
    catch { $res += "svrtest lan threw: $($_.Exception.Message)" }
    $json = ($res | Where-Object { $_ -match '^\s*[\{\}"]' }) -join "`n"
    $r = $null
    try { $r = $json | ConvertFrom-Json } catch { }
    $folder = if ($r -and $r.folder) { $r.folder } else { (Get-ChildItem $runs -Directory | Where-Object { $_.Name -like "*_$name" -and $_.CreationTime -gt $t0 } | Sort-Object CreationTime | Select-Object -Last 1).FullName }
    $ok = if ($r) { "ok $($r.ok) ($($r.why)); lockstep " + (($r.lockstep | ForEach-Object { "$($_.clock):$($_.same)" }) -join " ") + "; ports $($r.ports.server)/$($r.ports.host)/$($r.ports.join); errors host $($r.errors.host) join $($r.errors.join)" } else { "no result.json: " + (($res | Select-Object -Last 3) -join " / ") }
    $ok
    $block = @("===== $name ($(Get-Date -Format 'yyyy-MM-dd HH:mm')) link '$link' play $Play$(if ($Tunnel) { " tunnel" })$(if ($Lead) { " lead $Lead" })$(if ($GameArgs) { " " + ($GameArgs -join " ") })", $ok, "folder: $folder")
    if ($folder -and (Test-Path (Join-Path $folder "host.log"))) {
        $block += py -3 (Join-Path $d "lag_budget.py") $folder $hostIn $joinIn 2>&1 | ForEach-Object { $_.ToString() }
    }
    $block += ""
    $block | Add-Content (Join-Path $Out "budget.txt")
    $block | Select-Object -Skip 2
    # (a round with too many slow seconds - another chat's build or game ran through it - is run again)
    $slow = @($block | ForEach-Object { if ($_ -match "speed: (\d+) of (\d+) seconds under 57") { [int]$Matches[1] } }) | Measure-Object -Maximum
    if ($slow.Count -and $slow.Maximum -gt $SlowLimit -and $WaitQuiet) { "round $name ran slow ($($slow.Maximum) s under full speed): again"; continue }
    break
  }
}
Remove-Item Env:SVRTEST_SERVER_PORT_FROM -ErrorAction SilentlyContinue
"done: $(Join-Path $Out 'budget.txt')"
