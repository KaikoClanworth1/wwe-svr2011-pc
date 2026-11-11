# The lockstep lag budget rounds: for each link, one hidden two-game LAN round (svrtest lan: the host EDGE, the
# joiner KANE, both pressing every 3 s for -Play seconds, state dumps compared at the clocks) with both games'
# net input logs on, both games' outgoing packets delayed by the link (SVR2011_TEST_NET_DELAY "ms,jitter,loss%":
# one way = ms), then lag_budget.py over the round's folder. Results append to <Out>\budget.txt.
# The test server takes a port from 8530 (SVRTEST_SERVER_PORT_FROM, this chat's range).
# -Tunnel: both games through the tunnel (SVR2011_TEST_NET_TUNNEL=1: the internet path, each packet repeating the last
# 3 inputs so a lost one costs nothing); without it the LAN path (the game's socket straight to the peer).
#   .\lag_rounds.ps1 [-Links "", "80,20,2", "150,40,5"] [-Play 180s] [-Out "E:\SvR2011 Work\lag"] [-Tag a] [-Tunnel]
param([string[]]$Links = @("", "80,20,2", "150,40,5"), [string]$Play = "180s", [string]$Out = "E:\SvR2011 Work\lag",
      [string]$Tag = "", [switch]$Tunnel)
$ErrorActionPreference = "Continue"
$d = $PSScriptRoot
$runs = Join-Path (Split-Path (Split-Path $d -Parent) -Parent) "runs\svrtest"
$logs = Join-Path $Out "logs"
New-Item -ItemType Directory -Force $logs | Out-Null
$env:SVRTEST_SERVER_PORT_FROM = "8530"
foreach ($link in $Links) {
    $name = "lag_" + $(if ($link) { ($link -replace ",", "-") } else { "lan" }) + $(if ($Tag) { "_$Tag" } else { "" })
    $hostIn = Join-Path $logs "$name`_host_input.txt"; $joinIn = Join-Path $logs "$name`_join_input.txt"
    $envs = @("--host-env", "SVR2011_NET_INPUT_LOG=$hostIn", "--join-env", "SVR2011_NET_INPUT_LOG=$joinIn")
    if ($link) { $envs += @("--host-env", "SVR2011_TEST_NET_DELAY=$link", "--join-env", "SVR2011_TEST_NET_DELAY=$link") }
    if ($Tunnel) { $envs += @("--host-env", "SVR2011_TEST_NET_TUNNEL=1", "--join-env", "SVR2011_TEST_NET_TUNNEL=1") }
    $t0 = Get-Date
    "===== $name ($(Get-Date -Format 'yyyy-MM-dd HH:mm')) link '$link' play $Play$(if ($Tunnel) { " tunnel" })"
    $res = @()
    try { $res = & (Join-Path $d "svrtest_lan.ps1") --p1 EDGE --p2 KANE --play $Play --name $name --clock "600,1800,3600,5400,7200,9000" @envs 2>&1 | ForEach-Object { $_.ToString() } }
    catch { $res += "svrtest lan threw: $($_.Exception.Message)" }
    $json = ($res | Where-Object { $_ -match '^\s*[\{\}"]' }) -join "`n"
    $r = $null
    try { $r = $json | ConvertFrom-Json } catch { }
    $folder = if ($r -and $r.folder) { $r.folder } else { (Get-ChildItem $runs -Directory | Where-Object { $_.Name -like "*_$name" -and $_.CreationTime -gt $t0 } | Sort-Object CreationTime | Select-Object -Last 1).FullName }
    $ok = if ($r) { "ok $($r.ok) ($($r.why)); lockstep " + (($r.lockstep | ForEach-Object { "$($_.clock):$($_.same)" }) -join " ") + "; ports $($r.ports.server)/$($r.ports.host)/$($r.ports.join); errors host $($r.errors.host) join $($r.errors.join)" } else { "no result.json: " + (($res | Select-Object -Last 3) -join " / ") }
    $ok
    $block = @("===== $name ($(Get-Date -Format 'yyyy-MM-dd HH:mm')) link '$link' play $Play$(if ($Tunnel) { " tunnel" })", $ok, "folder: $folder")
    if ($folder -and (Test-Path (Join-Path $folder "host.log"))) {
        $block += py -3 (Join-Path $d "lag_budget.py") $folder $hostIn $joinIn 2>&1 | ForEach-Object { $_.ToString() }
    }
    $block += ""
    $block | Add-Content (Join-Path $Out "budget.txt")
    $block | Select-Object -Skip 2
}
Remove-Item Env:SVRTEST_SERVER_PORT_FROM -ErrorAction SilentlyContinue
"done: $(Join-Path $Out 'budget.txt')"
