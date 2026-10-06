# Crash hunt: boots into the training ring N times in the background (muted,
# never focused, windows off-screen), staying in the ring for a while, and
# reports each run's outcome (exit code of a crashed run, last log lines).
#   .\stress_ring.ps1 [-Runs 5] [-Native] [-Stay 60] [-Match]
# -Match: then from the ring's pause menu into a One on One match (entrances)
# and a minute of scripted fighting.
param([int]$Runs = 5, [switch]$Native, [int]$Stay = 60, [switch]$Match, [string]$Exe = "svr2011.exe", [string[]]$Extra = @())
$ErrorActionPreference = "Stop"
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class SWS {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
"@
$port  = Split-Path $PSScriptRoot -Parent
$game  = Join-Path (Split-Path $port -Parent) "Game Files"
$outDir = Join-Path $port "runs\stress"; New-Item -ItemType Directory -Force $outDir | Out-Null
$inputFile = Join-Path $outDir "input.txt"

function Send([string[]]$lines) { Add-Content $inputFile ($lines -join "`n") }

. (Join-Path $PSScriptRoot "test_guard.ps1")
$results = @()
for ($i = 1; $i -le $Runs; $i++) {
    Assert-NoPlayerGame
    Stop-TestGames
    Set-Content $inputFile "" -NoNewline
    $log = Join-Path $outDir ("run{0}{1}.log" -f $i, $(if ($Native) { "n" } else { "" }))
    Remove-Item $log -ErrorAction SilentlyContinue
    $env:SVR2011_INPUT_FILE = $inputFile
    $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
    $env:SVR2011_NATIVE_WINDOW_POS = "-2600,0"
    $a = @("--log_file=`"$log`"", "--log_level=info", "--audio_mute=true", "--fullscreen=false", "--monitor=2")
    if ($Native) { $a += "--native_renderer=shadow" }
    $a += $Extra
    Use-TestSaves
    $p = Start-Process (Join-Path $game $Exe) -WorkingDirectory $game -ArgumentList $a -PassThru -WindowStyle Minimized
    $handle = $p.Handle  # keeps the exit code available
    for ($t = 0; $t -lt 100 -and -not $p.HasExited; $t++) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 100 }
    if ($p.MainWindowHandle -ne 0) {  # off-screen, shown without activation (as session.ps1)
        [void][SWS]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 1920, 0, 0, 0, 0x1 -bor 0x10)  # (screen 2, behind every window)
        [void][SWS]::ShowWindow($p.MainWindowHandle, 4)
    }
    $stage = "boot"
    $steps = @(
        @{ wait = 40; stage = "title";  input = @("press START 200", "wait 6000", "press A 200") },
        @{ wait = 12; stage = "menu";   input = @("press A 200") },
        @{ wait = 30; stage = "select"; input = @("press A 200") },
        @{ wait = $Stay; stage = "ring"; input = @() })
    if ($Match) {
        $steps += @{ wait = 5; stage = "pause"; input = @("press START 200") }
        foreach ($k in 1..14) { $steps += @{ wait = 7; stage = "match setup $k"; input = @("press A 200") } }
        $fight = @("stick L 32767 0 1500", "press X 150", "wait 300", "press X 150", "press A 200", "wait 1500",
                   "press A 200", "press B 200", "stick L -32767 0 1000", "press Y 200", "press RB 200")
        foreach ($k in 1..6) { $steps += @{ wait = 10; stage = "fight $k"; input = $fight } }
    }
    foreach ($s in $steps) {
        $end = (Get-Date).AddSeconds($s.wait)
        while ((Get-Date) -lt $end -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }
        if ($p.HasExited) { break }
        $stage = $s.stage
        if ($s.input.Count) { Send $s.input }
    }
    if ($p.HasExited) {
        $code = "0x{0:X8}" -f $p.ExitCode
        $tail = (Get-Content $log -Tail 6) -join "`n    "
        $results += "run $i : CRASHED during '$stage' exit $code`n    $tail"
    } else {
        $results += "run $i : ok (reached '$stage')"
        Stop-Process -Id $p.Id -Force
        Wait-Process -Id $p.Id -Timeout 15 -ErrorAction SilentlyContinue
    }
    Write-Output $results[-1]
}
