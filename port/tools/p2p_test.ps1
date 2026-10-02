# P2P online tests: two background games on this PC (online_session.ps1
# slots "" and 2), one hosting a Player Match session, the other finding it
# (CUSTOM MATCH search) and joining. Screenshots runs\<Name>_*.png; logs
# runs\<Name>_host.log and runs\<Name>_join.log.
#   .\p2p_test.ps1 [-Name p2] [-HostOnly] [-JoinOnly] [-Play]
# -Play: then both pick a Superstar (the lobby's slots have a cursor: the
# joiner moves to 2P first) and the match starts (screenshots *_match*).
# Test saves and settings only: runs\test_userdata_ent_down + test_config_ent_down.toml
# (host, port base 36000), runs\test_userdata_ent_up + test_config_ent_up_p2.toml
# (joiner, port base 36100).
param([string]$Name = "p2", [switch]$HostOnly, [switch]$JoinOnly, [switch]$Play, [string]$LogLevel = "info")
$s = Join-Path $PSScriptRoot "online_session.ps1"
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
function P([string]$slot, [string]$b, [double]$w) {
    if ($slot) { & $s input -Slot $slot "press $b 200" | Out-Null } else { & $s input "press $b 200" | Out-Null }
    [Threading.Thread]::Sleep([int]($w * 1000))
}
function Shot([string]$slot, [string]$stem) {
    if ($slot) { & $s shot -Slot $slot $stem | Out-Null } else { & $s shot $stem | Out-Null }
}
# title -> main menu -> ONLINE -> (paint data prompt: NO) -> MATCH -> PLAYER MATCH
function ToPlayerMatch([string]$slot) {
    P $slot START 10; P $slot START 50; P $slot A 8; P $slot START 15
    P $slot DOWN 1.5; P $slot DOWN 1.5; P $slot DOWN 1.5; P $slot DOWN 1.5; P $slot A 25
    P $slot A 25
    P $slot A 10; P $slot DOWN 1.5; P $slot A 10
}
if (-not $JoinOnly) {
    & $s start -Name "$Name`_host" -UserData (Join-Path $runs "test_userdata_ent_down") -Config test_config_ent_down.toml -LogLevel $LogLevel | Out-Null
    [Threading.Thread]::Sleep(75000)
    ToPlayerMatch ""
    P "" DOWN 1.2; P "" DOWN 1.2; P "" A 10            # CREATE SESSION
    1..5 | ForEach-Object { P "" DOWN 0.8 }
    P "" A 15                                          # CONTINUE
    Shot "" "$Name`_host_lobby"
}
if (-not $HostOnly) {
    & $s start -Slot 2 -Name "$Name`_join" -UserData (Join-Path $runs "test_userdata_ent_up") -Config test_config_ent_up_p2.toml -LogLevel $LogLevel | Out-Null
    [Threading.Thread]::Sleep(75000)
    ToPlayerMatch "2"
    P "2" DOWN 1.5; P "2" A 15                         # CUSTOM MATCH
    P "2" DOWN 1; P "2" DOWN 1; P "2" A 15             # SEARCH FOR A MATCH
    Shot "2" "$Name`_join_list"
    P "2" A 15                                         # the session
    P "2" A 20                                         # JOIN: YES
    Shot "2" "$Name`_join_joined"
    Shot "" "$Name`_host_joined"
}
if ($Play) {
    P "" A 6; P "" A 3; P "" A 6                        # host: 1P, a Superstar, attire
    P "2" RIGHT 2; P "2" A 6; P "2" A 3; P "2" A 6      # joiner: 2P, a Superstar, attire
    Shot "" "$Name`_host_ready"
    foreach ($i in 1..4) { [Threading.Thread]::Sleep(25000); Shot "" "$Name`_host_match$i"; Shot "2" "$Name`_join_match$i" }
}
foreach ($log in "$Name`_host", "$Name`_join") {
    $f = Join-Path $runs "$log.log"
    if (Test-Path $f) {
        "--- $log"
        Get-Content $f | Select-String -CaseSensitive -Pattern "p2p:|net: bind|\) failed:|access violation" |
            Select-Object -Last 25 | ForEach-Object { $_.Line.Substring([math]::Min(45, $_.Line.Length)) }
    }
}
