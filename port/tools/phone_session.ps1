# Drives the game on the connected phone (USB debugging) for tests, as
# session.ps1 does on the PC: test saves (never the phone's own), a scripted
# controller (src/script_input.h) and muted audio.
#   phone_session.ps1 start                     (re)start the game for a test
#   phone_session.ps1 input "press START" "wait 2000" "press A"
#   phone_session.ps1 key <BUTTON> [-Wait 1.5] one press, then wait (menus drop fast presses)
#   phone_session.ps1 shot <stem>              screenshot -> port\runs\<stem>.png
#   phone_session.ps1 log [lines]              the end of the game's log
#   phone_session.ps1 stop
# Everything the test writes stays in games/WWE SmackDown vs. Raw 2011/test_run.
param([Parameter(Position = 0)][string]$Action,
      [Parameter(Position = 1, ValueFromRemainingArguments = $true)][string[]]$Rest,
      [double]$Wait = 1.5)
$ErrorActionPreference = "Continue"
$adb = "D:\Android\sdk\platform-tools\adb.exe"
if ($env:ANDROID_HOME -and (Test-Path (Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"))) {
    $adb = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
}
$pkg = "io.github.kaikoclanworth1.svr2011"
$game = "/storage/emulated/0/games/WWE SmackDown vs. Raw 2011"
$run = "$game/test_run"
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
$env:MSYS_NO_PATHCONV = "1"

function Sh([string]$cmd) { & $adb shell $cmd }
function Q([string]$path) { "'" + $path + "'" }

switch ($Action) {
    "start" {
        if (-not ((& $adb devices) -match "`tdevice")) { "no phone connected (USB debugging)"; return }
        & $adb shell am force-stop $pkg | Out-Null
        Sh "mkdir -p $(Q "$run/userdata")" | Out-Null
        # Test saves, once (the PC tests' saves and achievements).
        if (-not (Sh "ls $(Q "$run/userdata/Saves") 2>/dev/null")) {
            $src = Join-Path $runs "test_userdata"
            foreach ($d in "Saves", "achievements") { & $adb push (Join-Path $src $d) "$run/userdata/" | Out-Null }
        }
        Sh "rm -f $(Q "$run/input.txt"); touch $(Q "$run/input.txt")" | Out-Null
        & $adb shell am start -n "$pkg/.InstallActivity" `
            --es SVR2011_INPUT_FILE "'$run/input.txt'" `
            --es SVR2011_USER_DATA "'$run/userdata'" `
            --es args "--audio_mute=true" | Out-Null
        "started"
    }
    { $_ -in "input", "key" } {
        $lines = if ($Action -eq "key") { @("press $($Rest[0]) 200") } else { $Rest }
        $tmp = Join-Path $env:TEMP "svr2011_phone_input.txt"
        [IO.File]::WriteAllText($tmp, (($lines -join "`n") + "`n"))
        & $adb push $tmp "/data/local/tmp/svr2011_input.txt" | Out-Null
        Sh "cat /data/local/tmp/svr2011_input.txt >> $(Q "$run/input.txt")" | Out-Null
        if ($Action -eq "key") { Start-Sleep -Milliseconds ([int]($Wait * 1000)) } else { "queued: $($lines -join ' | ')" }
    }
    "shot" {
        # The Fold has two displays: keep the brighter (the one the game is on).
        $ids = (Sh "dumpsys SurfaceFlinger --display-id") | ForEach-Object { if ($_ -match '^Display (\d+)') { $Matches[1] } }
        $best = $null; $bestMean = -1
        Add-Type -AssemblyName System.Drawing
        foreach ($id in $ids) {
            $f = Join-Path $runs "phone_$id.png"
            cmd /c "`"$adb`" exec-out screencap -p -d $id > `"$f`""
            try {
                $b = [System.Drawing.Bitmap]::FromFile($f); $sum = 0; $n = 0
                for ($x = 20; $x -lt $b.Width; $x += 97) { for ($y = 20; $y -lt $b.Height; $y += 61) { $c = $b.GetPixel($x, $y); $sum += $c.R + $c.G + $c.B; $n++ } }
                $b.Dispose()
                if ($n -and $sum / $n -gt $bestMean) { $bestMean = $sum / $n; $best = $f }
            } catch {}
        }
        $out = Join-Path $runs "$($Rest[0]).png"
        if ($best) { Copy-Item $best $out -Force; $out } else { "no screenshot" }
    }
    "log" {
        $n = if ($Rest) { [int]$Rest[0] } else { 30 }
        # (each run has its own log in the game's logs folder: the newest)
        $log = (Sh "ls -t $(Q "$game/logs") | head -1").Trim()
        Sh "tail -n $n $(Q "$game/logs/$log")"
    }
    "stop" {
        & $adb shell am force-stop $pkg | Out-Null
        "stopped"
    }
    default { "usage: phone_session.ps1 start|input|key|shot|log|stop" }
}
