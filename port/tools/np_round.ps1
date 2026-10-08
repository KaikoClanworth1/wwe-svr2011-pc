# netperf: one measured round - two games on this PC (host 36000, joiner
# 36100) through the tunnel, a 1v1 Player Match (entrances off), 60 s of
# play, both stopped. The test settings are set here, so both games get them.
#   np_round.ps1 -Name np4 [-Delay "75,40,1"] [-Seconds 60]
# -Delay: SVR2011_TEST_NET_DELAY for both games (ms[,jitter[,loss%]]); "" none.
# -V1: the old tunnel packets only (no repeats): the "before" of a comparison.
param([string]$Name = "np", [string]$Delay = "", [int]$Seconds = 60, [switch]$V1,
      [string]$HostConfig = "test_config_ent_down.toml", [string]$JoinConfig = "test_config_ent_up_p2.toml")
$env:SVR2011_TEST_NET_V1 = if ($V1) { "1" } else { "" }
$s = Join-Path $PSScriptRoot "np_session.ps1"
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
$env:SVR2011_NET_STATS = "2"
$env:SVR2011_TEST_NET_TUNNEL = "1"
$env:SVR2011_TEST_NET_DELAY = $Delay
function In([string]$slot) { if ($slot) { & $s input -Slot $slot @args | Out-Null } else { & $s input @args | Out-Null } }
function Wait-Log([string]$log, [string]$pattern, [int]$after, [int]$timeout) {
    $f = Join-Path $runs "$log.log"
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $timeout) {
        if (@(Select-String -Path $f -Pattern $pattern -ErrorAction SilentlyContinue).Count -gt $after) { return $true }
        Start-Sleep 2
    }
    return $false
}
function Count([string]$log, [string]$pattern) { @(Select-String -Path (Join-Path $runs "$log.log") -Pattern $pattern -ErrorAction SilentlyContinue).Count }

# host: ONLINE -> MATCH (it loads slowly) -> PLAYER MATCH -> CREATE SESSION, entrance off, CONTINUE
& $s start -Name "$Name`_host" -UserData (Join-Path $runs "test_userdata_ent_down") -Config $HostConfig | Out-Null
In "" "title" "menu ONLINE" "wait 15000" "press A 200" "wait 40000" "press DOWN 200" "wait 1500" "press A 200" "wait 12000" `
     "press DOWN 200" "wait 1200" "press DOWN 200" "wait 1200" "press A 200" "wait 12000" `
     "press DOWN 200" "wait 900" "press DOWN 200" "wait 900" "press DOWN 200" "wait 900" "press RIGHT 200" "wait 1200" `
     "press DOWN 200" "wait 900" "press DOWN 200" "wait 900" "press A 200"
if (-not (Wait-Log "$Name`_host" "session .* hosted \(flags 42F" 0 300)) { "host: no session"; & $s shot "$Name`_host_fail"; return }
# joiner: ONLINE -> MATCH -> PLAYER MATCH -> QUICK MATCH -> YES
& $s start -Slot 2 -Name "$Name`_join" -UserData (Join-Path $runs "test_userdata_ent_up") -Config $JoinConfig | Out-Null
In "2" "title" "menu ONLINE" "wait 15000" "press A 200" "wait 40000" "press DOWN 200" "wait 1500" "press A 200" "wait 12000" `
     "press A 200" "wait 15000" "press A 200"
if (-not (Wait-Log "$Name`_host" "join remote" 0 300)) { "joiner: didn't join"; & $s shot -Slot 2 "$Name`_join_fail"; return }
Start-Sleep 12
# picks: the host 1P (the first Superstar), the joiner 2P, one to the right
In "" "press A 200" "wait 4000" "press A 200" "wait 3000" "press A 200" "wait 3000" "press A 200"
In "2" "press RIGHT 200" "wait 2000" "press A 200" "wait 4000" "press RIGHT 200" "wait 1500" "press A 200" "wait 3000" "press A 200" "wait 3000" "press A 200"
$starts = Count "$Name`_host" "svr2011\] match starts"
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 180 -and (Count "$Name`_host" "svr2011\] match starts") -le $starts) {
    In "" "press A 200"; In "2" "press A 200"; Start-Sleep 4
}
"match started after $([int]((Get-Date) - $t0).TotalSeconds) s"
# play: both move and strike
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    In "" "press LEFT 500" "press X 150"; In "2" "press RIGHT 500" "press A 150"; Start-Sleep 3
}
& $s shot "$Name`_host_match"; & $s shot -Slot 2 "$Name`_join_match"
& $s stop | Out-Null; & $s stop -Slot 2 | Out-Null
foreach ($log in "$Name`_host", "$Name`_join") {
    "===== $log"
    Select-String -Path (Join-Path $runs "$log.log") -Pattern "net sim|net: " | Select-Object -Last 12 |
        ForEach-Object { $_.Line.Substring([Math]::Min(40, $_.Line.Length)) }
}
