# PORT RULES two-game test (limits work): this session's own test server
# (127.0.0.1:8411, runs\lim_srv) and two background games from runs\lim_game
# (lim_session.ps1 slots "" and 2): the host picks WEAPONS EVERYWHERE on
# CREATE SESSION and hosts, the joiner searches and joins (the lobby panel),
# then both pick and the match starts (both should play rule 4D).
#   .\lim_p2p.ps1 [-Name lp1] [-RbPresses 1]
# The joiner's account (LimitTest2) is made on the test server the first time:
# runs\test_config_lim_online2.toml. Stops both games and the server at the end.
param([string]$Name = "lp1", [int]$RbPresses = 1)
$port = Split-Path $PSScriptRoot -Parent; $runs = Join-Path $port "runs"
$s = Join-Path $PSScriptRoot "lim_session.ps1"
$py = "C:\Users\Ranger\AppData\Local\Programs\Python\Python311\python.exe"
function In([string]$slot, [string[]]$keys) { & $s input -Slot $slot @keys | Out-Null }
function Shot([string]$slot, [string]$stem) { & $s shot -Slot $slot $stem | Out-Null }
function Low([string]$slot) {
    try { (Get-Process -Id ((Get-Content (Join-Path $runs "lim_session$slot.json") | ConvertFrom-Json).pid)).PriorityClass = "BelowNormal" } catch {}
}

$srv = Start-Process $py -ArgumentList "`"$port\server\server.py`" --host 127.0.0.1 --port 8411 --data `"$runs\lim_srv\data`" --log `"$runs\lim_srv\server.log`"" -PassThru -WindowStyle Hidden
try { $srv.PriorityClass = "BelowNormal" } catch {}
try {
    Start-Sleep 4
    $cfg2 = Join-Path $runs "test_config_lim_online2.toml"
    if (-not (Test-Path $cfg2)) {
        $pw = -join ((48..57) + (97..122) | Get-Random -Count 20 | ForEach-Object { [char]$_ })
        $r = Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:8411/api/register" -ContentType "application/json" -Body (@{ name = "LimitTest2"; password = $pw } | ConvertTo-Json)
        if (-not $r.ok) { throw "register: $($r.error)" }
        $t = (Get-Content (Join-Path $runs "test_config_lim_online.toml") -Raw) -replace 'online_token = ".*"', "online_token = `"$($r.token)`"" `
             -replace 'online_xuid = ".*"', "online_xuid = `"$($r.xuid)`"" -replace 'online_name = ".*"', 'online_name = "LimitTest2"'
        $t = $t.TrimEnd() + "`np2p_port = 36100`np2p_address = `"127.0.0.2`"`n"
        [IO.File]::WriteAllText($cfg2, $t)   # (no BOM)
    }
    $env:SVR2011_WINDOW_BEHIND = "1"
    $env:SVR2011_CONFIG = Join-Path $runs "test_config_lim_online.toml"; $env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
    & $s start -Name "$Name`_host" | Out-Null; Low ""
    $env:SVR2011_CONFIG = $cfg2; $env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim2"
    & $s start -Slot 2 -Name "$Name`_join" | Out-Null; Low "2"
    In "" @("route online"); In "2" @("route online")
    Start-Sleep 70
    # host: MATCH -> PLAYER MATCH -> CREATE SESSION, the PORT RULES row, CONTINUE
    In "" @("press A", "wait 2500", "press DOWN", "wait 800", "press A", "wait 2500", "press DOWN", "wait 700", "press DOWN", "wait 700", "press A", "wait 3000")
    Start-Sleep 16
    foreach ($i in 1..$RbPresses) { In "" @("press RB", "wait 1200") }
    Start-Sleep 4; Shot "" "$Name`_host_cs"
    In "" @("press DOWN", "wait 800", "press DOWN", "wait 800", "press DOWN", "wait 800", "press DOWN", "wait 800", "press DOWN", "wait 800", "press A", "wait 3000")
    Start-Sleep 16; Shot "" "$Name`_host_lobby"
    # joiner: MATCH -> PLAYER MATCH -> CUSTOM MATCH -> SEARCH -> the session -> YES
    In "2" @("press A", "wait 2500", "press DOWN", "wait 800", "press A", "wait 2500", "press DOWN", "wait 800", "press A", "wait 3000")
    Start-Sleep 12
    In "2" @("press DOWN", "wait 800", "press DOWN", "wait 800", "press A", "wait 3000"); Start-Sleep 14; Shot "2" "$Name`_join_list"
    In "2" @("press A", "wait 3000"); Start-Sleep 14; Shot "2" "$Name`_join_info"
    In "2" @("press A", "wait 3000"); Start-Sleep 18; Shot "2" "$Name`_join_lobby"; Shot "" "$Name`_host_joined"
    # both pick (the joiner moves to 2P first); the match starts
    In "" @("press A", "wait 6000", "press A", "wait 3000", "press A", "wait 6000")
    In "2" @("press RIGHT", "wait 2000", "press A", "wait 6000", "press A", "wait 3000", "press A", "wait 6000")
    foreach ($i in 1..3) { Start-Sleep 25; Shot "" "$Name`_host_match$i"; Shot "2" "$Name`_join_match$i" }
} finally {
    & $s stop | Out-Null; & $s stop -Slot 2 | Out-Null
    Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
    "games: " + (Get-CimInstance Win32_Process -Filter "Name='svr2011.exe'" | Where-Object { $_.CommandLine -match "runs\\lim_" } | Measure-Object).Count + "; 8411: " + [bool](Get-NetTCPConnection -State Listen -LocalPort 8411 -ErrorAction SilentlyContinue)
}
foreach ($log in "$Name`_host", "$Name`_join") {
    "--- $log"
    Select-String -Path (Join-Path $runs "$log.log") -Pattern "online rules|port rules|p2p: session|joined|weapons|crash|access violation" -ErrorAction SilentlyContinue |
        Select-Object -Last 25 | ForEach-Object { $_.Line.Substring([math]::Min(48, $_.Line.Length)) }
}
