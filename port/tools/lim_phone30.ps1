# Low-power 30 on the phone: a CPU-vs-CPU match in each mode, measured over
# -Measure s once the match clock runs - the game's CPU time (/proc), the
# battery current (dumpsys battery "current now", uA: + charging), the
# thermal service's temperatures, the game's "fps:" lines. Muted, test saves
# (phone_session.ps1). One line per mode; logs pulled to runs\lim_ph_<mode>.log.
#   .\lim_phone30.ps1 [-Modes m60,m30,own2,own1,saver30] [-Measure 60] [-Cool 45]
param([string[]]$Modes = @("m60", "m30", "own2", "own1", "saver30"), [int]$Measure = 60, [int]$Cool = 45,
      [string]$People = "JOHN CENA,RANDY ORTON", [string]$Tag = "ph")
$ErrorActionPreference = "Continue"
$adb = "D:\Android\sdk\platform-tools\adb.exe"
$pkg = "io.github.kaikoclanworth1.svr2011"
$game = "/storage/emulated/0/games/WWE SmackDown vs. Raw 2011"
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
$ps = Join-Path $PSScriptRoot "phone_session.ps1"
$env:MSYS_NO_PATHCONV = "1"
function Sh([string]$c) { & $adb shell $c }
function NewestLog { (Sh "ls -t '$game/logs' | head -1").Trim() }
function Temps {
    $t = @{}; $on = $false
    foreach ($l in (Sh "dumpsys thermalservice")) {
        if ($l -match "Current temperatures from HAL") { $on = $true; continue }
        if ($on -and $l -match "mValue=([0-9.]+), mType=\d+, mName=(\w+)") { if (-not $t.ContainsKey($Matches[2])) { $t[$Matches[2]] = [double]$Matches[1] } }
        elseif ($on -and $l -match "^\S") { break }
    }
    $t
}
function Current { $l = Sh "dumpsys battery | grep 'current now'"; if ("$l" -match "(-?\d+)") { [long]$Matches[1] } else { 0 } }
function Jiffies($procId) { $s = (Sh "cat /proc/$procId/stat") -replace '^.*\) ', ''; $f = $s -split ' '; [long]$f[11] + [long]$f[12] }

$saver = "--arena_crowd=off --native_shadows=false --native_render_scale=0.5 --native_texture_quality=low"
foreach ($mode in $Modes) {
    $args_ = switch ($mode) {
        "m60" { "--frame_rate=60" } "m30" { "--frame_rate=30" } "own1" { "--frame_rate=60 --test_own_30=1" }
        "own2" { "--frame_rate=60 --test_own_30=2" } "saver30" { "--frame_rate=30 $saver" } "saver60" { "--frame_rate=60 $saver" }
    }
    $env:SVR2011_PHONE_ARGS = "--full_speed=true $args_"
    $env:SVR2011_TEST_MATCH = "people=$People arena=1 cpu=all"
    $env:SVR2011_FPS_PROBE = "1"
    $r = & $ps start
    if ("$r" -notmatch "started") { "$mode : $r"; break }
    Start-Sleep 8
    & $ps input "route normal" "pressuntil A test match: people" | Out-Null
    $log = NewestLog
    # the match: the probe's match frames over 20 a second, 3 s running
    $ok = $false; $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 420) {
        Start-Sleep 5
        $at = @(Sh "grep -n '\] test match: people' '$game/logs/$log'")
        if (-not $at.Count -or "$($at[0])" -notmatch '^(\d+):') { continue }
        $from = [int]$Matches[1]
        $n = 0
        foreach ($l in @(Sh "grep -n 'fps probe: ' '$game/logs/$log' | tail -3")) {
            if ($l -match '^(\d+):.*probe: (\d+) game' -and [int]$Matches[1] -gt $from -and [long]$Matches[2] -gt 20 -and [long]$Matches[2] -lt 200) { $n++ }
        }
        if ($n -ge 3) { $ok = $true; break }
    }
    if (-not $ok) { "$mode : no match start"; & $ps shot "lim_${Tag}_${mode}_stuck" | Out-Null; & $ps stop | Out-Null; continue }
    $gpid = (Sh "pidof $pkg").Trim()
    $T0 = Temps; $j0 = Jiffies $gpid; $w0 = Get-Date
    $cur = @()
    while (((Get-Date) - $w0).TotalSeconds -lt $Measure) { $cur += Current; Start-Sleep 2 }
    $j1 = Jiffies $gpid; $wall = ((Get-Date) - $w0).TotalSeconds; $T1 = Temps
    & $ps shot "lim_${Tag}_$mode" | Out-Null
    & $ps stop | Out-Null
    $local = Join-Path $runs "lim_${Tag}_$mode.log"
    & $adb pull "$game/logs/$log" $local 2>$null | Out-Null
    $fps = @(Get-Content $local | Select-String "\] fps: ([0-9.]+) avg" | Select-Object -Last ([int]($Measure / 5) - 1) | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
    $ft = @(Get-Content $local | Select-String "frame times: median ([0-9.]+) ms, p90 ([0-9.]+), p99 ([0-9.]+)" | Select-Object -Last 1)
    $probe = @(Get-Content $local | Select-String "fps probe: (\d+) game frames" | Select-Object -Last ($Measure + 2) | Select-Object -First $Measure)
    $live = @($probe | Where-Object { [long]$_.Matches[0].Groups[1].Value -gt 20 -and [long]$_.Matches[0].Groups[1].Value -lt 200 }).Count
    $avgCur = [math]::Round((($cur | Measure-Object -Average).Average) / 1000)
    $dt = { param($k) if ($T0[$k] -and $T1[$k]) { "{0:N1}->{1:N1}" -f $T0[$k], $T1[$k] } else { "?" } }
    "{0,-8} cpu {1,4:N0}% of a core | battery {2} mA (+ charging) | AP {3} SKIN {4} BAT {5} | fps {6:N1} | match ran {8}/{9} s | {7}" -f $mode,
        (100 * ($j1 - $j0) / 100 / $wall), $avgCur, (& $dt "AP"), (& $dt "SKIN"), (& $dt "BAT"),
        $(if ($fps.Count) { ($fps | Measure-Object -Average).Average } else { 0 }), $(if ($ft) { $ft[0].Line.Substring($ft[0].Line.IndexOf("frame times")) } else { "" }), $live, $probe.Count
    Start-Sleep $Cool
}
foreach ($v in "SVR2011_PHONE_ARGS", "SVR2011_TEST_MATCH", "SVR2011_FPS_PROBE") { Remove-Item "env:$v" -ErrorAction SilentlyContinue }
