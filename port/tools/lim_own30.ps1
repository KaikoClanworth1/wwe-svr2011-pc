# Low-power 30 research: CPU use of a CPU-vs-CPU match in each frame-rate
# mode (one game at a time, muted, screen 2 behind every window, below normal).
#   .\lim_own30.ps1 [-Modes m60,m30,half30,own1,own2] [-Measure 20] [-Tag x]
# Each mode: launch runs\lim_game (route match, cpu=all), wait for the match,
# settle, then the process's CPU over -Measure s, the fps probe and 3 shots.
param([string[]]$Modes = @("m60", "m30", "half30", "own1", "own2"), [int]$Measure = 20, [string]$Tag = "o30",
      [string]$People = "JOHN CENA,RANDY ORTON")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class WO {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[void][WO]::SetProcessDPIAware()
. (Join-Path $PSScriptRoot "test_guard.ps1")
$port = Split-Path $PSScriptRoot -Parent
$game = Join-Path $port "runs\lim_game"
$runs = Join-Path $port "runs"

function Shot($p, $png) {
    $p.Refresh(); $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return }
    $r = New-Object WO+RECT; [void][WO]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][WO]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
    $s = [math]::Min(1.0, 960.0 / $w)
    $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w * $s)), ([int]($hh * $s))
    $small.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $small.Dispose(); $g.Dispose(); $bmp.Dispose()
}

$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = "0"; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = "0"
Use-TestSaves
foreach ($mode in $Modes) {
    Assert-NoPlayerGame
    $name = "lim_${Tag}_$mode"
    $log = Join-Path $runs "$name.log"; Remove-Item $log -ErrorAction SilentlyContinue
    $env:SVR2011_TEST_HALF_30 = $null; $env:SVR2011_TEST_OWN_30 = $null
    $fps = 60
    switch ($mode) {
        "m30"    { $fps = 30 }
        "half30" { $fps = 30; $env:SVR2011_TEST_HALF_30 = "1" }
        "own1"   { $env:SVR2011_TEST_OWN_30 = "1" }
        "own2"   { $env:SVR2011_TEST_OWN_30 = "2" }
    }
    $env:SVR2011_TEST_MATCH = "people=$People arena=1 cpu=all"
    $env:SVR2011_FPS_PROBE = "1"
    $in = Join-Path $runs "lim_o30_input.txt"; Set-Content $in "route normal`npressuntil A test match: people" -Encoding ascii
    $env:SVR2011_INPUT_FILE = $in
    $env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
    $env:SVR2011_NATIVE_WINDOW_POS = "-2600,0"
    $a = @("--log_file=`"$log`"", "--log_level=info", "--audio_mute=true", "--fullscreen=false", "--monitor=2",
           "--frame_rate=$fps", "--full_speed=true")
    $p = Start-Process (Join-Path $game "svr2011.exe") -WorkingDirectory $game -ArgumentList $a -PassThru -WindowStyle Minimized
    for ($t = 0; $t -lt 100; $t++) { $p.Refresh(); if ($p.HasExited -or $p.MainWindowHandle -ne [IntPtr]::Zero) { break }; Start-Sleep -Milliseconds 100 }
    if (-not $p.HasExited) {
        try { $p.PriorityClass = "BelowNormal" } catch {}
        [void][WO]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, $SvrParkX, 0, 0, 0, $SvrParkFlags)
        [void][WO]::ShowWindow($p.MainWindowHandle, 4)
    }
    # the match: its rule line, then 15 s to settle (entrances off or over)
    $ok = $false
    $skip = $false
    for ($t = 0; $t -lt 600 -and -not $p.HasExited; $t++) {
        Start-Sleep 1
        if (-not (Test-Path $log)) { continue }
        $rule = Select-String -Path $log -Pattern "\] test match: people" | Select-Object -First 1
        if (-not $rule) { continue }
        $run = @(Select-String -Path $log -Pattern "fps probe: (\d+) game frames" | Where-Object { $_.LineNumber -gt $rule.LineNumber } |
                 ForEach-Object { [long]$_.Matches[0].Groups[1].Value })
        if ($run.Count -ge 3 -and ($run[-3..-1] | Where-Object { $_ -gt 20 -and $_ -lt 200 }).Count -eq 3) { $ok = $true; break }  # (the match clock runs)
    }
    if (-not $ok) { "$mode : no match start ($(if ($p.HasExited) { 'exited' } else { 'timeout' }))"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; continue }
    Start-Sleep 5
    Shot $p (Join-Path $runs "${name}_1.png")
    $p.Refresh(); $cpu0 = $p.TotalProcessorTime.TotalMilliseconds; $t0 = Get-Date
    $lines0 = (Get-Content $log).Count
    Start-Sleep ([int]($Measure / 2)); Shot $p (Join-Path $runs "${name}_2.png"); Start-Sleep ($Measure - [int]($Measure / 2))
    $p.Refresh(); $cpu1 = $p.TotalProcessorTime.TotalMilliseconds; $wall = ((Get-Date) - $t0).TotalMilliseconds
    Shot $p (Join-Path $runs "${name}_3.png")
    $probe = Get-Content $log | Select-Object -Skip $lines0 | Select-String -Pattern "fps probe: (\d+) game frames/s, (\d+) shown"
    $gf = @($probe | ForEach-Object { [long]$_.Matches[0].Groups[1].Value }); $sh = @($probe | ForEach-Object { [long]$_.Matches[0].Groups[2].Value })
    $avg = { param($v) if ($v.Count) { [math]::Round(($v | Measure-Object -Average).Average, 1) } else { 0 } }
    Stop-Process -Id $p.Id -Force; Wait-Process -Id $p.Id -Timeout 15 -ErrorAction SilentlyContinue
    "{0,-7} cpu {1,5:N0}% of a core | match frames/s {2} | shown/s {3} | probe {4}" -f $mode, (100 * ($cpu1 - $cpu0) / $wall),
        (& $avg $gf), (& $avg $sh), (($gf | ForEach-Object { $_ }) -join ",")
    Start-Sleep 3
}
foreach ($v in "SVR2011_ROUTE", "SVR2011_TEST_MATCH", "SVR2011_FPS_PROBE", "SVR2011_TEST_HALF_30", "SVR2011_TEST_OWN_30", "SVR2011_INPUT_FILE", "SVR2011_CONFIG", "SVR2011_NATIVE_WINDOW_POS") { Remove-Item "env:$v" -ErrorAction SilentlyContinue }
