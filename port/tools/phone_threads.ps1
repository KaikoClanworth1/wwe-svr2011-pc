# CPU per thread of the game on the connected phone (USB debugging), from
# Android's top over -Seconds: which threads (XMA decoder, GPU commands, the
# game's) cost what there. Starts the game if it isn't running; leave it on
# the title screen or in a match (the attract demo has music, crowd and
# commentary).
#   phone_threads.ps1 [-Seconds 10] [-Top 20]
param([int]$Seconds = 10, [int]$Top = 20)
$adb = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
if (-not $env:ANDROID_HOME -or -not (Test-Path $adb)) { $adb = "D:\Android\sdk\platform-tools\adb.exe" }
$pkg = "io.github.kaikoclanworth1.svr2011"
if (-not ((& $adb devices) -match "`tdevice")) { "no phone connected (USB debugging)"; return }
$gamePid = (& $adb shell pidof $pkg).Trim()
if (-not $gamePid) {
    & $adb shell am start -n "$pkg/.InstallActivity" | Out-Null
    "started the game - run again once it's on the title screen or in a match"
    return
}
# Two samples: the second is the CPU use over -Seconds.
$out = & $adb shell top -H -b -d $Seconds -n 2 -p $gamePid -o TID,%CPU,CPU,TIME+,THREAD
$blocks = ($out -join "`n") -split "(?m)^Tasks:"
$last = ($blocks[-1] -split "`n") | Where-Object { $_ -match '^\s*\d+\s' }
"CPU per thread over ${Seconds}s (100 = one core; CPU = the core it last ran on):"
"  TID   %CPU CORE  TIME+     THREAD"
$last | Sort-Object { [double](($_ -split '\s+', 0, 'RegexMatch' | Where-Object { $_ })[1]) } -Descending |
    Select-Object -First $Top
