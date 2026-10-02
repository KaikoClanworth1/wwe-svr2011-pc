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
# -Relay: both through the online server's match relay (port\server\relay.py,
# in the test server at 127.0.0.1:8421) instead of directly: the *_relay.toml
# configs (p2p_force_relay), as two players who can't reach each other would play.
param([string]$Name = "p2", [switch]$HostOnly, [switch]$JoinOnly, [switch]$Play, [switch]$Relay, [switch]$Fight,
      [string]$LogLevel = "info", [string]$HostConfig = "", [string]$JoinConfig = "",
      [string]$HostUserData = "")
# -HostConfig: the host's settings instead (e.g. a test account on the public
# server, with -HostOnly, for a phone to join: tools/phone_session.ps1).
# -JoinConfig: the joiner's (e.g. another frame rate: sync tests). -HostUserData: the
# host's saves folder in runs (e.g. a copy with Created Superstars).
# -Fight: in the match both players move and strike for a while, then both
# games are screenshot at the same moment (runs\<Name>_*_sync*.png: the
# referee and both Superstars should stand in the same places).
$s = Join-Path $PSScriptRoot "online_session.ps1"
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
$suffix = ""
if ($Relay) { $suffix = "_relay" }
function P([string]$slot, [string]$b, [double]$w) {
    if ($slot) { & $s input -Slot $slot "press $b 200" | Out-Null } else { & $s input "press $b 200" | Out-Null }
    [Threading.Thread]::Sleep([int]($w * 1000))
}
function Shot([string]$slot, [string]$stem) {
    if ($slot) { & $s shot -Slot $slot $stem | Out-Null } else { & $s shot $stem | Out-Null }
}
# title -> main menu -> ONLINE -> (paint data prompt: NO) -> MATCH -> PLAYER MATCH
function ToPlayerMatch([string]$slot, [string]$log) {
    # START at the title loads the saves ("rows added to the match menus" in the
    # log: the loading ring, its exhibition loading behind); one START at the
    # wrong moment goes back to the title, so START until the saves load. The
    # ring then waits for START for the main menu (not A: that starts its
    # exhibition match) once the exhibition has loaded.
    $f = Join-Path $runs "$log.log"
    $before = @(Select-String -Path $f -Pattern "rows added to the match menus" -ErrorAction SilentlyContinue).Count
    foreach ($try in 1..10) {
        P $slot START 12
        if (@(Select-String -Path $f -Pattern "rows added to the match menus" -ErrorAction SilentlyContinue).Count -gt $before) { break }
    }
    [Threading.Thread]::Sleep(25000)
    P $slot START 15
    P $slot DOWN 1.5; P $slot DOWN 1.5; P $slot DOWN 1.5; P $slot DOWN 1.5; P $slot A 25
    P $slot A 25
    P $slot A 10; P $slot DOWN 1.5; P $slot A 10
}
if (-not $JoinOnly) {
    & $s start -Name "$Name`_host" -UserData $(if ($HostUserData) { Join-Path $runs $HostUserData } else { Join-Path $runs "test_userdata_ent_down" }) -Config $(if ($HostConfig) { $HostConfig } else { "test_config_ent_down$suffix.toml" }) -LogLevel $LogLevel | Out-Null
    [Threading.Thread]::Sleep(75000)
    ToPlayerMatch "" "$Name`_host"
    P "" DOWN 1.2; P "" DOWN 1.2; P "" A 10            # CREATE SESSION
    1..5 | ForEach-Object { P "" DOWN 0.8 }
    P "" A 15                                          # CONTINUE
    Shot "" "$Name`_host_lobby"
}
if (-not $HostOnly) {
    & $s start -Slot 2 -Name "$Name`_join" -UserData (Join-Path $runs "test_userdata_ent_up") -Config $(if ($JoinConfig) { $JoinConfig } else { "test_config_ent_up_p2$suffix.toml" }) -LogLevel $LogLevel | Out-Null
    [Threading.Thread]::Sleep(75000)
    ToPlayerMatch "2" "$Name`_join"
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
if ($Fight) {
    # (pairs of presses, one game then the other; the referee follows them around)
    $moves = @(@("LEFT 700", "RIGHT 500"), @("X 150", "UP 600"), @("UP 400", "X 150"), @("RIGHT 900", "LEFT 300"),
               @("A 150", "DOWN 700"), @("DOWN 500", "A 150"))
    foreach ($round in 1..3) {
        foreach ($m in $moves) {
            & $s input "press $($m[0])" | Out-Null
            & $s input -Slot 2 "press $($m[1])" | Out-Null
            [Threading.Thread]::Sleep(1500)
        }
        [Threading.Thread]::Sleep(4000)
        # (both at once: a second apart they'd differ anyway)
        $jobs = @(
            Start-Job { param($s, $n) & $s shot $n } -ArgumentList $s, "$Name`_host_sync$round"
            Start-Job { param($s, $n) & $s shot -Slot 2 $n } -ArgumentList $s, "$Name`_join_sync$round")
        $jobs | Wait-Job | Remove-Job
    }
}
foreach ($log in "$Name`_host", "$Name`_join") {
    $f = Join-Path $runs "$log.log"
    if (Test-Path $f) {
        "--- $log"
        Get-Content $f | Select-String -CaseSensitive -Pattern "p2p:|relay:|net: bind|\) failed:|access violation" |
            Select-Object -Last 25 | ForEach-Object { if ($_.Line.StartsWith("[")) { $_.Line.Substring([math]::Min(45, $_.Line.Length)) } else { $_.Line } }
    }
}
