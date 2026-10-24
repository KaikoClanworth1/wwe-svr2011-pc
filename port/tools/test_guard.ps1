# Dot-sourced by the test scripts: they must never touch a game the player
# started. A test game is one launched with a log file under port\runs.
#   Stop-TestGames       closes only test games
#   Assert-NoPlayerGame  throws when a game not started by a test is running

# Test games open behind every other window (the SDK's SVR2011_WINDOW_BEHIND,
# before the window is ever shown) on the second screen (--monitor=2): the player
# watching something never sees one pop up.
$env:SVR2011_WINDOW_BEHIND = "1"
# One screen (the player's TV): no test window at all - the SDK never shows the
# window (SVR2011_WINDOW_HIDDEN); the game still draws, frame shots
# (SVR2011_TEST_FRAME_SHOT) read its back buffer. Window screenshots (PrintWindow)
# of a hidden window come out black: use frame shots.
$env:SVR2011_WINDOW_HIDDEN = "1"

# Where a test window is parked: the second screen if there is one (x 1920 +),
# else it stays where it opened on the only screen - behind every window
# (HWND_BOTTOM), never activated. Off every screen, the game's own pages
# don't draw. $SvrParkX / $SvrParkFlags: SetWindowPos(h, 1, $SvrParkX + dx, dy, 0, 0, $SvrParkFlags).
Add-Type -AssemblyName System.Windows.Forms
$SvrTwoScreens = @([System.Windows.Forms.Screen]::AllScreens | Where-Object { $_.Bounds.X -ge 1920 }).Count -gt 0
$SvrParkX = if ($SvrTwoScreens) { 1920 } else { 0 }
$SvrParkFlags = if ($SvrTwoScreens) { 0x11 } else { 0x13 }   # NOSIZE|NOACTIVATE (+ NOMOVE on one screen)

function Get-SvrGames {
    @(Get-CimInstance Win32_Process -Filter "Name = 'svr2011.exe' OR Name = 'svr2011_trace.exe' OR Name = 'svr2011_dbg.exe'" -ErrorAction SilentlyContinue)
}

# A test folder of any chat: its exe's folder or a parent holds the (empty) marker file .svr2011_test - e.g.
# "D:\Xbox Games Ports\SvR2011 Anthology Test" - so a test run from outside port\runs is never taken for the
# player's game. (Those games still count toward the 4 test games at once; Stop-TestGames leaves them alone.)
function Test-HasTestMarker([string]$exe) {
    if (-not $exe) { return $false }
    $d = Split-Path $exe -Parent
    while ($d) {
        if (Test-Path -LiteralPath (Join-Path $d ".svr2011_test")) { return $true }
        $up = Split-Path $d -Parent
        if (-not $up -or $up -eq $d) { break }
        $d = $up
    }
    return $false
}

function Test-IsTestGame($proc) {
    $cmd = [string]$proc.CommandLine
    return ($cmd -match [regex]::Escape("\port\runs\")) -or (Test-HasTestMarker ([string]$proc.ExecutablePath))
}

function Stop-TestGames {
    foreach ($p in Get-SvrGames) {
        # (never a game run from its own copy under port\runs - onl_game,
        # netperf_game, online_game, lim_game, opt_*...: another session's test,
        # with up to 6 test games from several sessions at once)
        if ([string]$p.ExecutablePath -match [regex]::Escape("\port\runs\")) { continue }
        if (Test-HasTestMarker ([string]$p.ExecutablePath)) { continue }  # (another chat's marked test folder)
        # (not the online tests' games: online_session.ps1 runs them beside others;
        # nor the Limit Breaking and Optimizing sessions' games: lim_game, opt_*)
        if ((Test-IsTestGame $p) -and ([string]$p.CommandLine -notmatch [regex]::Escape("\runs\online_game\")) -and
            ([string]$p.CommandLine -notmatch [regex]::Escape("\runs\lim_game\")) -and
            ([string]$p.CommandLine -notmatch [regex]::Escape("\runs\opt_"))) {
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
            Wait-Process -Id $p.ProcessId -Timeout 15 -ErrorAction SilentlyContinue
        }
    }
}

function Assert-NoPlayerGame {
    # A process that is shutting down reports no command line: look again
    # before taking such a one for the player's.
    for ($try = 0; $try -lt 5; $try++) {
        $player = @(Get-SvrGames | Where-Object { -not (Test-IsTestGame $_) })
        if (-not $player.Count) { return }
        if (@($player | Where-Object { $_.CommandLine }).Count) { break }
        Start-Sleep -Seconds 2
    }
    throw "The game is running (pid $($player[0].ProcessId), not started by a test) - not starting a test while someone is playing."
}

# Test games keep their saves in port\runs\test_userdata (SVR2011_USER_DATA),
# never in the player's Game Files\UserData: a test that saves (autosave after a
# match, a created superstar) must not overwrite the player's created content.
# Seeded once from a copy of the player's saves; the installed DLC is linked in
# (the game only reads it). Keeps a value already set (caw_save_test.ps1).
function Use-TestSaves {
    if ($env:SVR2011_USER_DATA) { return }
    $port = Split-Path $PSScriptRoot -Parent
    $player = Join-Path (Split-Path $port -Parent) "Game Files\UserData"
    $ud = Join-Path $port "runs\test_userdata"
    if (-not (Test-Path $ud)) {
        New-Item -ItemType Directory $ud | Out-Null
        Get-ChildItem $player -Directory | Where-Object { $_.Name -match '^[0-9A-F]{16}$' -and $_.Name -ne '0000000000000000' } |
            ForEach-Object { Copy-Item -Recurse $_.FullName $ud }
    }
    # The installed DLC as a private copy, never a link: game code that writes
    # user data would otherwise change the player's own files (an older run
    # left a junction here - replaced).
    $dlc = Join-Path $player "0000000000000000"
    $mine = Join-Path $ud "0000000000000000"
    $item = Get-Item $mine -ErrorAction SilentlyContinue
    if ($item -and $item.LinkType) { cmd /c rmdir "$mine" | Out-Null; $item = $null }
    if (-not $item -and (Test-Path $dlc)) {
        Copy-Item -Recurse $dlc $mine
        Get-ChildItem $mine -Recurse -Filter *.orig | Remove-Item
    }
    $env:SVR2011_USER_DATA = $ud
}
