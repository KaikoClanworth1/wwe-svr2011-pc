# Overnight Android runner (docs/OVERNIGHT_TESTS.md, "Android"): runs
# runs\night\android_scenarios.jsonl (android_scenarios.py) on the phone, one
# game at a time, as night.ps1 does on the PC. Writes
# runs\night\android_results.jsonl (one line per scenario; a rerun skips those
# already there unless -Redo), the logs to runs\night\android_logs, screenshots
# to runs\night\android_shots.
# Test saves and user data only (games/.../test_run), muted. The phone's own
# settings file is checked against its state at the start and put back.
#   android_night.ps1 [-Only B,D] [-Ids B00,D17] [-Redo] [-Serial RFGL105N6GJ]
param([string[]]$Only = @(), [string[]]$Ids = @(), [switch]$Redo, [string]$Serial = "RFGL105N6GJ",
      [string]$Scenarios = "")
$ErrorActionPreference = "Continue"
Add-Type -AssemblyName System.Drawing
$env:ANDROID_SERIAL = $Serial
$env:MSYS_NO_PATHCONV = "1"
$adb = "D:\Android\sdk\platform-tools\adb.exe"
$pkg = "io.github.kaikoclanworth1.svr2011"
$game = "/storage/emulated/0/games/WWE SmackDown vs. Raw 2011"
$run = "$game/test_run"
$port = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$night = Join-Path $port "runs\night"
$results = Join-Path $night "android_results.jsonl"
$logs = Join-Path $night "android_logs"; $shots = Join-Path $night "android_shots"
New-Item -ItemType Directory -Force $logs, $shots | Out-Null
if (-not $Scenarios) { $Scenarios = Join-Path $night "android_scenarios.jsonl" }
$phoneSession = Join-Path $port "tools\phone_session.ps1"

function Sh([string]$cmd) { & $adb shell $cmd 2>$null }
function Q([string]$p) { "'" + $p + "'" }

# ---- the phone ---------------------------------------------------------------
function Wait-Phone {
    $said = $false
    while ($true) {
        $ok = ((& $adb devices) -match "$Serial`tdevice") -and
              (((Sh "dumpsys power | grep mWakefulness=") -join " ") -match "Awake") -and
              -not (Sh "dumpsys window | grep isKeyguardShowing=true")
        if ($ok) { return }
        if (-not $said) { "{0} waiting for the phone (connected, awake, unlocked)" -f (Get-Date -Format HH:mm:ss); $said = $true }
        Start-Sleep -Seconds 30
    }
}

# Settings file at the start (put back at the end).
$tomlBackup = "$run/toml_before_night.txt"
Sh "mkdir -p $(Q $run)" | Out-Null
Sh "cp $(Q "$game/svr2011.toml") $(Q $tomlBackup)" | Out-Null

function Newest-Log { ((Sh "ls -t $(Q "$game/logs") | grep 'log`$' | head -1") -join "").Trim() }

$errPattern = 'SvR 2011 crash|\[FATAL\]|Unhandled guest access|device lost|DEVICE_REMOVED|can''t draw the game|not in its list|failed to load'

function Start-Scenario($s) {
    Wait-Phone
    & $adb shell am force-stop $pkg | Out-Null
    Start-Sleep -Seconds 1
    $before = Newest-Log
    Sh "mkdir -p $(Q "$run/userdata")" | Out-Null
    # test saves once (phone_session.ps1 start does the same)
    if (-not (Sh "ls $(Q "$run/userdata/Saves") 2>/dev/null")) {
        $src = Join-Path $port "runs\test_userdata"
        foreach ($d in "Saves", "achievements") { & $adb push (Join-Path $src $d) "$run/userdata/" | Out-Null }
    }
    Sh "rm -f $(Q "$run/input.txt"); touch $(Q "$run/input.txt")" | Out-Null
    $extras = @()
    foreach ($p in $s.env.PSObject.Properties) {
        $v = [string]$p.Value
        if ($p.Name -eq "SVR2011_TEST_ARENA_REDIRECT") { $v = $v -replace '\\', '/' }
        if ($p.Name -eq "SVR2011_MUSIC") { $v = "$run/$v" }
        $extras += @("--es", $p.Name, "'$v'")
    }
    $args_ = (@("--audio_mute=true", "--show_fps=false") + @($s.args)) -join " "
    & $adb shell am start -n "$pkg/.InstallActivity" @extras `
        --es SVR2011_INPUT_FILE "'$run/input.txt'" --es SVR2011_USER_DATA "'$run/userdata'" `
        --es args "'$args_'" | Out-Null
    # its log: the newest one, once it isn't the one from before
    $log = ""
    for ($i = 0; $i -lt 30 -and (-not $log -or $log -eq $before); $i++) { Start-Sleep -Seconds 1; $log = Newest-Log }
    return @{ s = $s; log = $log; start = Get-Date; lines = 0; tmStarted = $false; passed = $false; ends = 0
              err = @(); stuckMax = 0; fps = @(); worst = @(); slow = 0; frames = 0; shots = @(); nextShot = 0
              lastFpsAt = Get-Date }
}

function Read-New($r) {
    if (-not $r.log) { return }
    $new = Sh "sed -n '$($r.lines + 1),`$p' $(Q "$game/logs/$($r.log)")"
    foreach ($line in @($new)) {
        if ($null -eq $line) { continue }
        $r.lines++
        if ($line -match 'test match: ') { $r.tmStarted = $true }
        if ($r.tmStarted -and $line -match $r.s.pass) {
            $r.passed = $true; $r.ends++
            if ($r.s.soak) { & $phoneSession input "route match" | Out-Null }  # (the next match)
        }
        if ($line -match $errPattern -and $r.err.Count -lt 12) { $r.err += $line.Substring(0, [Math]::Min(240, $line.Length)) }
        if ($line -match 'world update stuck (\d+) s') { $r.stuckMax = [Math]::Max($r.stuckMax, [int]$Matches[1])
            if ($r.err.Count -lt 12) { $r.err += $line.Substring(0, [Math]::Min(240, $line.Length)) } }
        if ($line -match 'test match: no superstar') { $r.err += $line.Substring(0, [Math]::Min(200, $line.Length)) }
        if ($line -match 'fps: ([\d.]+) avg, worst frame ([\d.]+) ms \((\d+) frames.*<35ms (\d+) / <51ms (\d+) / longer (\d+)') {
            $r.fps += [double]$Matches[1]; $r.worst += [double]$Matches[2]; $r.lastFpsAt = Get-Date
            $r.frames += [int]$Matches[3]; $r.slow += [int]$Matches[5] + [int]$Matches[6]
        }
    }
}

function Shot($r) {
    $stem = "{0}_{1}" -f $r.s.id, $r.nextShot
    $p = (& $phoneSession shot "night_$stem" 2>$null | Select-Object -Last 1)
    $src = Join-Path $port "runs\night_$stem.png"
    if (-not (Test-Path $src)) { return $null }
    $dst = Join-Path $shots "$stem.png"
    Move-Item $src $dst -Force
    # brightness: a black / one-colour picture
    $b = [System.Drawing.Bitmap]::FromFile($dst)
    $vals = @()
    for ($y = 0; $y -lt 24; $y++) { for ($x = 0; $x -lt 40; $x++) {
        $c = $b.GetPixel([int](($x + 0.5) * $b.Width / 40), [int](($y + 0.5) * $b.Height / 24))
        $vals += (0.299 * $c.R + 0.587 * $c.G + 0.114 * $c.B) } }
    $b.Dispose()
    $mean = ($vals | Measure-Object -Average).Average
    $sd = [math]::Sqrt((($vals | ForEach-Object { ($_ - $mean) * ($_ - $mean) }) | Measure-Object -Average).Average)
    return [ordered]@{ path = $dst; mean = [math]::Round($mean, 1); sd = [math]::Round($sd, 1) }
}

function Finish($r, [string]$result) {
    Read-New $r
    & $adb shell am force-stop $pkg | Out-Null
    if ($r.log) { & $adb pull "$game/logs/$($r.log)" (Join-Path $logs "$($r.s.id).log") 2>$null | Out-Null }
    $secs = [int]((Get-Date) - $r.start).TotalSeconds
    $fpsAfter = if ($r.fps.Count -gt 3) { $r.fps[3..($r.fps.Count - 1)] } else { $r.fps }
    $row = [ordered]@{
        id = $r.s.id; suite = $r.s.suite; name = $r.s.name; result = $result; seconds = $secs; device = "Fold"
        errors = $r.err; stuckMax = $r.stuckMax; matchesEnded = $r.ends
        fpsMinAvg = if ($fpsAfter.Count) { ($fpsAfter | Measure-Object -Minimum).Minimum } else { $null }
        fpsMeanAvg = if ($fpsAfter.Count) { [math]::Round(($fpsAfter | Measure-Object -Average).Average, 1) } else { $null }
        worstFrameMs = if ($r.worst.Count) { ($r.worst | Measure-Object -Maximum).Maximum } else { $null }
        framesOver35msPct = if ($r.frames) { [math]::Round(100.0 * $r.slow / $r.frames, 2) } else { $null }
        shots = $r.shots; log = "android_logs\$($r.s.id).log"; time = (Get-Date -Format s)
    }
    ($row | ConvertTo-Json -Compress -Depth 4) | Add-Content $results
    "{0} {1,-22} {2,-8} {3,5}s  {4}" -f (Get-Date -Format HH:mm:ss), $r.s.id, $result, $secs, $r.s.name
}

# ---- the scenarios -------------------------------------------------------------
$done = @{}
if ((Test-Path $results) -and -not $Redo) {
    foreach ($l in Get-Content $results) { try { $done[($l | ConvertFrom-Json).id] = $true } catch {} }
}
$list = Get-Content $Scenarios | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object {
    (-not $Only.Count -or $Only -contains $_.suite) -and (-not $Ids.Count -or $Ids -contains $_.id) -and -not $done[$_.id] }
"{0} {1} scenarios to run" -f (Get-Date -Format HH:mm:ss), @($list).Count

foreach ($s in $list) {
    $r = Start-Scenario $s
    $result = $null
    while (-not $result) {
        Start-Sleep -Seconds 5
        Read-New $r
        $age = ((Get-Date) - $r.start).TotalSeconds
        if ($r.nextShot -lt $s.shots.Count -and $age -ge $s.shots[$r.nextShot]) {
            $sh = $null; try { $sh = Shot $r } catch {}
            if ($sh) { $r.shots += $sh }
            $r.nextShot++
        }
        $alive = [bool]((Sh "pidof $pkg") -join "")
        if (-not $alive) { $result = if ($r.passed -and -not $s.soak) { "pass" } else { "crash" } }
        elseif ($r.err | Where-Object { $_ -match 'SvR 2011 crash|FATAL' }) { $result = "crash" }
        elseif ($r.err | Where-Object { $_ -match 'device lost|DEVICE_REMOVED|can''t draw' }) { $result = "gpu" }
        elseif ($r.stuckMax -ge 30) { $result = "hang" }
        elseif ($r.passed -and -not $s.soak) { $result = "pass" }
        elseif ($r.err | Where-Object { $_ -match 'test match: no superstar' }) { $result = "invalid" }
        elseif ($age -gt $s.timeout) { $result = if ($s.soak -and $r.ends -gt 0) { "pass" } else { "timeout" } }
        elseif ($age -gt 90 -and ((Get-Date) - $r.lastFpsAt).TotalSeconds -gt 40) { $result = "hang" }
    }
    if ($result -eq "pass" -and ($r.shots | Where-Object { $_.sd -lt 3 -and $_.mean -lt 10 }).Count -ge 2) { $result = "black" }
    Finish $r $result
}

# Put the phone's settings back.
& $adb shell am force-stop $pkg | Out-Null
Sh "cp $(Q $tomlBackup) $(Q "$game/svr2011.toml")" | Out-Null
"{0} all done -> {1}" -f (Get-Date -Format HH:mm:ss), $results
